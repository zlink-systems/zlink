# Rust STREAM·REQREP 비용 지도 (Core 0.17.3-alpha)

이 기록은 Rust `MULTI_STREAM`의 크기와 무관한 고정 비용을 C와 비교해 찾고, 첫 번째
지배 항목만 줄인 결과를 남긴다. REQREP은 Single 러너가 정책의 실행 모델을 지키는지 먼저
검사하고, 그 결과를 Multi 재측정과 함께 분리해 기록한다.

## 결론

- STREAM의 지배 항목은 Rust 서버가 socket 입력을 기다리지 않고 최대 1 ms의
  `thread::park_timeout`을 반복한 것이다. 64 B에서 C 대비 9,277.2 ns였던 메시지당 격차 중
  9,192.9 ns, 곧 99.1%가 public `Poller`로 입력과 completion을 함께 기다리자 사라졌다.
- STREAM pass 1 뒤 Rust는 64~65,536 B에서 동시각 C의 96.1~98.7%에 도달했다. 수정 전
  87~89 Kops/s였던 64~1,024 B는 458~459 Kops/s가 됐다.
- REQREP Single Rust 러너는 요청 하나를 만든 직후 reply가 올 때까지 poller에서 기다린다.
  따라서 정책이 금지하는 in-flight 1 RTT 루프이며, 현재 Single 수치는 binding 고유 비용을
  나타내지 않는다. 이 정합 문제를 먼저 보고하고 REQREP 코드는 수정하지 않았다.
- REQREP Multi의 새 동시각 짝측정에서는 64~4,096 B가 C의 54.2~62.7%였다. Single 러너가
  바르지 않으므로 그 profile을 Multi 격차의 원인으로 옮겨 붙일 수 없다. 남은 Multi 격차는
  `Request` future/completion runtime 비용과 공개 API 계약을 다시 분리해야 하는 미확인 항목이다.

## 측정 조건과 판정 방법

모든 공식 측정과 profile은 `perf-ticket.sh submit -p 2 -o codex`로 실행했다. 모든 새 측정은
다음 Core를 사용했다.

```text
ZLINK_CORE_SOURCE=release
ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.3-alpha
```

공식 측정은 tcp, 1 run이며 STREAM Multi는 100 clients를 사용했다. timeout, sleep, client 수,
크기, HWM과 상한은 바꾸지 않았다. before와 after 모두 각각 C를 먼저 실행하고 곧바로 Rust를
실행했다. 계획서의 Core 0.17.2 값은 비교에 쓰지 않았다.

`ns/msg`와 `ns/op`는 각각 `10^9 / throughput`이다. profile은 64 B의 짧은 instrumented run에서
호출 횟수와 할당을 세는 용도로만 썼다. profile의 throughput은 공식 처리량으로 쓰지 않았다.

Single 정책은 STREAM을 측정 대상에서 제외한다
(`doc/perf/PERF_SINGLE_TEST_POLICY.md:533`). 실제로 C와 Rust 모두 Single STREAM 실행 파일이
없다. 그러므로 STREAM은 요청에서 지정한 Multi 서버 경로를 직접 비교했다. REQREP은 Single을
먼저 비교했다.

## STREAM before 비용 지도

### 공식 동시각 짝측정

| 크기 | C (ops/s) | Rust (ops/s) | Rust/C | C (ns/msg) | Rust (ns/msg) | 격차 (ns/msg) |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 466,678.2 | 87,566.0 | 18.76% | 2,142.8 | 11,420.0 | 9,277.2 |
| 256 | 474,507.2 | 87,082.0 | 18.35% | 2,107.5 | 11,483.4 | 9,376.0 |
| 1,024 | 455,726.6 | 88,612.8 | 19.44% | 2,194.3 | 11,285.1 | 9,090.8 |
| 65,536 | 58,300.8 | 15,503.4 | 26.59% | 17,152.4 | 64,502.0 | 47,349.6 |

새 prefix의 동시각 짝측정에서는 65,536 B C 처리량도 58.3 Kops/s였다. 따라서 요청에 적힌
과거의 “크기와 무관하게 C 약 465 Kops/s, Rust 약 88 Kops/s”와 직접 비교하지 않는다.
64~1,024 B에서만 보면 Rust의 약 11.3~11.5 us 고정 간격과 약 9.1~9.4 us의 C 대비 격차가
재현됐다.

### 64 B profile 지도

호출 횟수의 분모는 성공한 echo send다. C는 13,622건, Rust는 13,898건이었다. 할당은 DHAT의
호출 stack을 application packet 작성과 Rust task 관리로 좁혀 센 값이다.

| 항목 | C | Rust before | C 대비 잔여 비용 | 64 B 격차 비중 | 성격 |
|---|---:|---:|---:|---:|---|
| 입력 대기 간격 | native poller wait 0.1017회/msg | socket 입력과 연결되지 않은 `park_timeout` 0.0307회/msg | pass 1 차분으로 9,192.9 ns/msg | 99.1% | 러너 결함, 지배 항목 |
| application 할당 | 121 B, 1회/msg | 777 B, 5회/msg | +656 B, +4회/msg | 독립 ns 미분리; pass 1 뒤 남은 격차의 상한은 84.3 ns/msg(0.9%) | runtime |
| public native 함수 진입 | 16.56회/msg | 17.22회/msg | +0.66회/msg | 독립 ns 미분리; 위 잔여 상한에 포함 | 일부 계약, 일부 runtime |
| Rust task 관리 | 해당 없음 | 겹침을 뺀 약 1,358 Ir/msg | 독립 ns 미분리 | pass 1 뒤 20% 미만 | runtime |
| application OS thread 이동 | 0회/msg | 0회/msg | 0 | 0% | 같은 전용 thread에서 실행 |
| blocking wake 관측 | poller 0.1017회/msg | 입력 readiness wake 0회, blind park 0.0307회/msg | 입력 도착이 server thread를 깨우지 못함 | 지배 항목의 호출 근거 | runtime 배선 |

할당 656 B의 구성은 future `Box` 480 B, wake용 `Arc` 48 B, ready queue 64 B, poll 결과 `Vec`
64 B다. 공통 packet frame 121 B는 양쪽에 각각 1회 있다. instruction 수는 시간을 더해서
환산하지 않았다. Callgrind instruction과 실제 대기 시간은 서로 더할 수 없기 때문이다.

native 함수 진입 횟수는 다음과 같다.

| 함수 묶음 | C (회/msg) | Rust (회/msg) |
|---|---:|---:|
| `zlink_stream_recv_packet` | 1.102 | 1.031 |
| `zlink_stream_send_packet` | 1.000 | 1.000 |
| message init/close/size/data/init_size/copy 합계 | 14.36 | 15.19 |
| `zlink_poller_wait` | 0.102 | 0.000 |
| 합계 | 16.56 | 17.22 |

Rust의 retry용 packet staging 때문에 생기는 init/copy/close 일부는 public `Message` 소유권과
비동기 send 계약에 걸려 있다. 이번 pass의 지배 항목이 아니므로 건드리지 않았다. Callgrind로
같은 application thread에서 future를 poll한다는 것은 확인했지만, completion reactor에서 즉시
끝난 작업과 실제로 thread를 깨운 작업을 분리할 수는 없었다. 따라서 그 cross-thread wake 횟수는
`미확인`으로 남기고, 관측 가능한 blocking wait만 표에 적었다.

## STREAM 지배 항목과 pass 1

수정 전 서버는 `recv_packet(DONT_WAIT)`와 send future 확인이 모두 빈 경우
`ConcurrentTasks::wait_for_wake(1 ms)`를 호출했다. 이 wake는 send future에는 연결됐지만 STREAM
socket의 `POLLIN`에는 연결되지 않았다. 새 packet이 도착해도 timeout 만료 전에는 다음
`recv_packet`을 호출하지 못했다. C 서버는 같은 경우 native poller에서 `POLLIN |
POLLCOMPLETION`을 기다렸다.

pass 1은 `bindings/rust/perf/multi/src/perf_multi_stream_server.rs` 한 파일만 바꿨다.

- public `Poller`에 STREAM socket을 `POLLIN | POLLCOMPLETION`으로 등록했다.
- active와 종료 drain의 기존 대기 지점에서 같은 poller를 기다리게 했다.
- timeout, drain deadline, HWM, client 수, 크기, send 방식과 public API signature는 바꾸지 않았다.

수정 전에는 “send future wake용 wait”와 “socket 입력을 확인하는 주기적 timeout”이라는 두 규칙이
있었다. 수정 뒤에는 “socket의 입력과 completion을 한 poller에서 기다린다”는 규칙 하나만 남았다.
규칙 수는 2개에서 1개로 줄었다.

### before/after

| 크기 | before C | before Rust | after C | after Rust | after Rust/C | Rust 증가 | before 격차 제거 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 466,678.2 | 87,566.0 | 476,759.6 | 458,339.4 | 96.14% | +423.42% | 99.09% |
| 256 | 474,507.2 | 87,082.0 | 469,543.0 | 458,743.4 | 97.70% | +426.79% | 99.47% |
| 1,024 | 455,726.6 | 88,612.8 | 464,805.2 | 458,665.4 | 98.68% | +417.61% | 99.68% |
| 65,536 | 58,300.8 | 15,503.4 | 60,039.0 | 58,770.4 | 97.89% | +279.08% | 99.24% |

64 B after의 C는 2,097.5 ns/msg, Rust는 2,181.8 ns/msg로 남은 격차는 84.3 ns/msg다.
after callgrind에서 `park_timeout`은 사라졌고, `zlink_poller_wait`는 1,233회/11,014 msg =
0.112회/msg였다. C before의 0.102회/msg와 같은 종류의 readiness 대기로 수렴했다.

## REQREP Single: 러너 정합 검사와 예비 지도

### 정책 위반

정책은 admission이 끝나면 reply를 기다리지 말고 다음 request를 계속 제출하며, public terminal이
admission과 reply를 하나의 awaitable로 제공하면 applied SNDHWM bytes를 wire size로 나눈 크기까지
미완료 집합을 유지하도록 요구한다
(`doc/perf/PERF_SINGLE_TEST_POLICY.md:69-124`, D-BP40).

Rust `run_reqrep`은 전용 OS thread에서 synchronous poll을 수행하므로 thread 소유 규칙은 지킨다.
그러나 `bindings/rust/perf/single/src/common.rs:753-839`에서 loop마다 request를 하나만 추가한 뒤,
그 future가 첫 poll에서 pending이면 곧바로 `poller.wait`를 호출한다. 첫 reply가 오기 전에는 다음
loop와 다음 제출로 가지 못한다. `RequestFuture`도
`bindings/rust/src/runtime/messaging/operations/routed_async.rs:93-168`에서 admission과 reply를 한
future로 합친다. 따라서 현재 러너는 정책에서 명시적으로 금지한 in-flight 1 RTT loop다.

이 문제를 고치려면 applied SNDHWM과 wire size로 미완료 집합의 경계를 정해야 한다. 이번 요청은
상한 변경과 인위적 in-flight cap을 금지했고, public `Request` future는 admission 결과를 밖으로
내보내지 않는다. 임의의 숫자 cap이나 timeout을 추가하지 않고 정합 문제만 먼저 보고한다.

### 공식 Single 짝측정

| 패턴 | 크기 | C (ops/s) | Rust (ops/s) | Rust/C | C (ns/op) | Rust (ns/op) | 격차 (ns/op) |
|---|---:|---:|---:|---:|---:|---:|---:|
| DR | 64 | 783,965.6 | 13,812.8 | 1.76% | 1,275.6 | 72,396.6 | 71,121.1 |
| DR | 256 | 832,095.8 | 13,807.2 | 1.66% | 1,201.8 | 72,426.0 | 71,224.2 |
| DR | 1,024 | 707,331.8 | 13,637.0 | 1.93% | 1,413.8 | 73,329.9 | 71,916.2 |
| DR | 4,096 | 279,975.4 | 13,061.8 | 4.67% | 3,571.7 | 76,559.1 | 72,987.4 |
| DR | 65,536 | 11,137.4 | 2,439.0 | 21.90% | 89,787.6 | 410,004.1 | 320,216.5 |
| RR | 64 | 888,618.6 | 13,887.2 | 1.56% | 1,125.3 | 72,008.8 | 70,883.4 |
| RR | 256 | 820,169.2 | 13,734.2 | 1.67% | 1,219.3 | 72,810.9 | 71,591.7 |
| RR | 1,024 | 694,555.6 | 13,609.0 | 1.96% | 1,439.8 | 73,480.8 | 72,041.0 |
| RR | 4,096 | 259,596.8 | 13,609.4 | 5.24% | 3,852.1 | 73,478.6 | 69,626.5 |
| RR | 65,536 | 11,169.6 | 2,245.4 | 20.10% | 89,528.7 | 445,355.0 | 355,826.2 |

64~4,096 B Rust의 약 72~76 us/op는 크기와 거의 무관하며, 공식 Rust mean latency
71.9~76.4 us와 거의 같다. 곧 throughput이 한 request의 RTT 역수로 제한됐다. 반면 C는 여러
request를 제출하고 completion을 drain하므로 mean latency와 throughput의 역수가 같지 않다.

### 64 B callgrind·DHAT 예비 지도

| 항목 | C | Rust | C 대비 | 격차 비중·판정 |
|---|---:|---:|---:|---|
| request part native 진입 | 2.000회/submission | 2.000회/submission | 0 | 공통 계약 |
| completion drain native 진입 | 1.016회/submission | 1.506회/submission | +0.490회 | runtime, cadence 영향 포함 |
| blocking poller wait | 0.0159회/submission | 1.001회/submission | +0.985회 | 약 71,121 ns/op의 Single 격차를 사실상 전부 만드는 실행 모델 차이 |
| active application thread 이동 | 0 | 0 | 0 | 양쪽 모두 전용 OS thread |
| requester 측 관리 할당 | 재사용 runner buffer 기준 0 B, 0회 | 816 B, 4회/op | +816 B, +4회 | ns 독립 미분리; 지배 항목 판정 불가 |

C callgrind run은 active 완료 837건 동안 request 11,392건을 제출했고, poller wait는 181회였다.
Rust는 완료 1,961건에 제출 1,966건, poller wait 1,968회였다. C는 admission과 reply를 분리해
연속 제출하지만 Rust는 사실상 완료마다 한 번 기다린다는 직접 근거다.

Rust DHAT의 816 B/4회는 future `Box` 496 B, `CompletionEntry` 88 B, payload `Message` 104 B,
poller drain 결과 `Vec` 128 B다. C DHAT 실행은 `FAIL`을 반환했으므로 native Core heap까지 포함한
C/Rust 총 할당 비교에는 쓰지 않았다. C 열의 0 B는 C runner가 request마다 새로 만드는 관리
객체가 없고 payload buffer를 재사용한다는 좁은 의미다.

Single의 약 71 us 격차는 binding의 한 연산 비용이 아니라 잘못된 동시 진행 규칙이 만든 값이다.
따라서 이 예비 profile에서 allocation이나 native 진입을 20% 지배 항목으로 판정해 구현하지 않았다.

## REQREP Multi 확인

Single 정합 문제와 별개로, 고정 Core에서 100-client Multi의 현재 격차를 같은 시각 C와 다시 쟀다.

| 패턴 | 크기 | C (ops/s) | Rust (ops/s) | Rust/C | C (ns/op) | Rust (ns/op) | 격차 (ns/op) |
|---|---:|---:|---:|---:|---:|---:|---:|
| DR | 64 | 359,278.2 | 221,468.4 | 61.64% | 2,783.4 | 4,515.3 | 1,732.0 |
| DR | 256 | 349,529.8 | 189,256.2 | 54.15% | 2,861.0 | 5,283.8 | 2,422.9 |
| DR | 1,024 | 339,557.4 | 205,677.2 | 60.57% | 2,945.0 | 4,862.0 | 1,917.0 |
| DR | 4,096 | 292,460.0 | 174,659.8 | 59.72% | 3,419.3 | 5,725.4 | 2,306.1 |
| DR | 65,536 | 55,442.6 | 16,062.6 | 28.97% | 18,036.7 | 62,256.4 | 44,219.8 |
| RR | 64 | 335,783.8 | 202,475.2 | 60.30% | 2,978.1 | 4,938.9 | 1,960.8 |
| RR | 256 | 315,760.0 | 185,588.8 | 58.78% | 3,167.0 | 5,388.3 | 2,221.3 |
| RR | 1,024 | 301,961.8 | 183,299.2 | 60.70% | 3,311.7 | 5,455.6 | 2,143.9 |
| RR | 4,096 | 280,033.0 | 175,543.2 | 62.69% | 3,571.0 | 5,696.6 | 2,125.6 |
| RR | 65,536 | 52,917.4 | 16,250.6 | 30.71% | 18,897.4 | 61,536.2 | 42,638.8 |

64~4,096 B의 잔여 격차는 1.73~2.42 us/op다. 65,536 B의 42.6~44.2 us/op는 크기 의존성이
강하므로 별도 항목이다. Multi에는 client가 100개 있어 Single의 in-flight 1 문제가 그대로
적용된다고 볼 수 없다. 정확한 원인은 Single 러너를 정책에 맞춘 뒤 다시 profile해야 한다.

## DD·SS 참고 셀의 상태

요청에서 별도 항목으로 지정한 두 SENDSEND 패턴의 4,096 B 초 단위 latency와 DD 처리량을 같은
alpha prefix로 다시 확인하려고, C와 Rust의
`DEALER_DEALER,DEALER_ROUTER_SENDSEND,ROUTER_ROUTER_SENDSEND` 5개 크기 짝측정 ticket
`2-1788844036-39455...`을 제출했다. 그러나 14:04에 perf queue runner가 종료됐고, ticket은
실행되지 않은 채 `.artifacts/perf-queue/pending/`에 남아 있다. ticket 도구가 runner는 감독자가
띄워야 한다고 반환했으므로 직접 benchmark를 실행하거나 lock을 우회하지 않았다.

따라서 요청에 적힌 Core 0.17.2의 SS 비율과 4,096 B 초 단위 latency를 이 문서의 alpha 비용
표에 섞지 않는다. 이 두 latency 이상은 STREAM pass 1이나 REQREP 처리량의 지배 항목으로
판정하지 않았고, 관련 코드를 수정하지도 않았다. runner가 다시 시작되면 pending ticket의
결과를 이 항목에 보완해야 한다.

## 결과와 경로

### 공식 report

- STREAM before C:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_133751_rust_costmap_before_c.txt`
- STREAM before Rust:
  `bindings/rust/perf/results/multi/report/perf_rust_multi_linux_20260908_133812_rust_costmap_before_rust.txt`
- STREAM after C:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_134610_rust_stream_pass1_after_c.txt`
- STREAM after Rust:
  `bindings/rust/perf/results/multi/report/perf_rust_multi_linux_20260908_134631_rust_stream_pass1_after_rust.txt`
- REQREP Single C:
  `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_134325_rust_costmap_single_before_c.txt`
- REQREP Single Rust:
  `bindings/rust/perf/results/single/report/perf_rust_single_linux_20260908_134416_rust_costmap_single_before_rust.txt`
- REQREP Multi C:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_135056_rust_reqrep_multi_costmap_c.txt`
- REQREP Multi Rust:
  `bindings/rust/perf/results/multi/report/perf_rust_multi_linux_20260908_135152_rust_reqrep_multi_costmap_rust.txt`

### profile과 ticket

- STREAM before 공식 측정: ticket `2-1788842264-49934...`, rc 0
- STREAM before callgrind·DHAT: ticket `2-1788842469-75002...`, rc 0
- STREAM after 공식 측정: ticket `2-1788842757-2545...`, rc 0
- STREAM after callgrind: ticket `2-1788842984-27007...`, rc 0,
  `/tmp/zlink-rust-stream-costmap/rust-callgrind-after-64.cg`
- REQREP Single 공식 측정: ticket `2-1788842505-78883...`, rc 0
- REQREP Single callgrind: ticket `2-1788842551-86958...`; C·Rust callgrind는 완료했고 뒤의 C DHAT
  benchmark가 실패해 ticket rc 1. profile은
  `/tmp/zlink-rust-stream-costmap/{c,rust}-single-dr-64.cg`다.
- REQREP Rust DHAT 재실행: ticket `2-1788842888-19330...`, rc 0,
  `/tmp/zlink-rust-stream-costmap/rust-single-dr-64.dhat.json`
- REQREP Multi 공식 측정: ticket `2-1788843038-35710...`, rc 0
- DD·SS 참고 셀: ticket `2-1788844036-39455...`, 미실행 pending. perf queue runner 종료로 대기

`/tmp` profile은 일시 파일이며 재부팅 뒤 유지된다고 보장하지 않는다. 재현 명령과 stdout/stderr는
각 ticket의 `.artifacts/perf-queue/log/` 파일에 남아 있다.

## 검증과 남은 격차 분류

- `cargo fmt --manifest-path bindings/rust/perf/multi/Cargo.toml -- --check`: 통과
- `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml`: 통과. 각 binary의 공통 test
  15~16개가 모두 통과했다(ticket `2-1788843260-67097...`).
- `cargo test --manifest-path bindings/rust/Cargo.toml`: pass 1과 무관한 기존 고정 Core assertion으로
  완료하지 못했다. 직렬 재실행에서도
  `request_future_preserves_more_than_1024_reply_parts`가
  `socket_base_api.cpp:1641 Assertion failed: released`로 SIGABRT했다. 앞선 test들은 통과했으며
  ticket은 `2-1788843260-67097...`, rc 1이다. Core는 요청 범위 밖이므로 수정하지 않았다.

남은 격차는 다음처럼 분류한다.

| 범위 | 분류 | 근거·다음 판단 |
|---|---|---|
| STREAM 64~65,536 B | runtime 잔여, 일부 공개 계약 | C의 96.1~98.7%, 64 B 잔여 84.3 ns/msg라 20% 지배 항목 없음 |
| REQREP Single | 러너 정합 결함 + 공개 API 계약 | admission/reply 결합 future와 in-flight 1 loop. 먼저 applied-HWM 기준 실행 모델을 정해야 함 |
| REQREP Multi 64~4,096 B | 미확인 runtime/계약 | 1.73~2.42 us/op. 바르지 않은 Single profile에서 원인을 추정하지 않음 |
| REQREP Multi 65,536 B | 미확인 크기 의존 항목 | 42.6~44.2 us/op. 작은 크기와 분리해 다뤄야 함 |
| DD·SS Multi 참고 셀 | 측정 대기 | alpha 짝측정 ticket이 runner 종료로 pending; 과거 0.17.2 수치는 혼용하지 않음 |
