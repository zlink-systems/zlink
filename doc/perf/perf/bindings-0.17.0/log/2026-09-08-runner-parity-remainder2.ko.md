# 러너 정합 잔여 3건 — Node relay teardown, Single one-way, C DR REQREP 64 B

## 결론

- Node routed SENDSEND relay의 reply 제출은 blocking 호출이 아니라 `submit()` Promise를 기다리는 async 경로였다. 다만 STOP 뒤 `RoutedReplySender.drain()`은 퇴장한 peer의 WRITABLE을 무한히 기다릴 수 있었다. 이를 server shutdown 예산 안의 bounded drain으로 바꿨고 20/20 run과 64 B 회귀를 통과했다.
- Go·Rust·Python Single one-way 5 pattern은 모두 전용 OS thread가 진행하며 one-way latency를 `/2`로 나누지 않았다. 수신 방식은 서로 달랐다. Go의 blocking-first recv, Rust의 deadline 뒤 1 ms poll, Python의 50 ms deadline poll을 C와 같은 `POLLIN(-1) -> DONTWAIT drain -> wire STOP` 한 규칙으로 통일했다.
- Rust에서는 이 변경으로 STOP의 잘못된 2-part shape와 sender socket 조기 drop이 드러났다. STOP을 single-part로 보내고 sender socket을 receiver의 STOP 관측까지 살려 해결했다. Python ROUTER/ROUTER에서는 인자를 받지 않는 `submit_sync()`에 `flags=`를 넘겨 sender thread가 죽던 결함을 고쳤다.
- C `DEALER_ROUTER_REQREP` tcp 64 B의 기존 실패는 고정 Core 0.17.3에서 5/5 재현되지 않았다. 실행 모델은 바꾸지 않았고, 다음 실패부터 case stderr와 requester/replier 종료 상태가 report에 남도록 증거 보존만 보강했다.

고정 조건은 `ZLINK_CORE_SOURCE=release`, `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.3`이다. 모든 perf 실행은 `perf-ticket.sh submit -p 2 -o codex-parity2`로 수행했다.

## A. Node routed SENDSEND relay teardown

### 원인과 수정

- `bindings/node/perf/multi/perf_multi_routed_sendsend.ts:113-118`: `sendServerReply()`는 `sendRouted(...).submit()`의 Promise를 `await`한다. Java처럼 async admission이며 Node event loop 자체를 blocking하지 않는다.
- 같은 파일 `:173-211`, `:548-564`: FIFO의 마지막 Promise를 STOP 뒤 무조건 기다리던 규칙이 퇴장한 route에서 끝나지 않을 수 있었다. `drainUntil()`과 pending count를 추가해 `min(PERF_MULTI_SEND_DRAIN_TIMEOUT_MS, server shutdown 예산 - 2 s)`, 하한 250 ms로 제한했다. 기본값은 3,000 ms다.
- 창 만료는 stderr에 `window_ms`와 `pending_replies`를 남긴다. 그 뒤 기존 `finally`의 socket close가 admission Promise를 종료한다. active 구간, client의 `EchoReplyDrain`, client 수/HWM/크기에는 손대지 않았다.

수정 전 규칙은 “STOP 뒤 reply Promise를 무기한 drain” 하나였고, 수정 후는 “STOP 뒤 shutdown 예산 안에서만 drain하고 close로 넘김” 하나다. 별도 route generation이나 in-flight cap은 추가하지 않았다.

### 검증

| pattern | 4096 B median | 65536 B median | 5-run 종료 |
|---|---:|---:|---:|
| `MULTI_DEALER_ROUTER_SENDSEND` | 81,689.8 msg/s | 27,558.6 msg/s | 10/10 |
| `MULTI_ROUTER_ROUTER_SENDSEND` | 89,963.0 msg/s | 23,072.6 msg/s | 10/10 |

- 본 측정: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260908_160633_nodeteardown.txt`, aggregate success 4, fail 0, status complete. 두 크기 × 두 pattern × 5 runs = 20/20이다.
- 64 B 회귀: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260908_160653_nodeteardown-64.txt`, DR 143,647.4 msg/s, RR 158,544.0 msg/s, 2/2 complete.
- 최신 같은 Core 0.17.3 기준 `perf_node_multi_linux_20260908_144712_m173.txt`의 DR 129,633.8, RR 149,154.4 msg/s 대비 각각 +10.8%, +6.3%다. -5% 회귀 한계를 만족한다.
- 계약 테스트 12/12 통과. `npm run build:incremental`로 `dist-tools`를 갱신했다.

## B. Go·Rust·Python Single one-way §1.1 감사

### C 기준

`bindings/c/perf/single/common/perf_single_one_way.hpp:288-360`의 기준은 다음 한 줄이다.

| 대상 | 진행 주체 | terminal | yield | recv 모델 | timestamp | active 경계 | throughput |
|---|---|---|---|---|---|---|---|
| C 5 pattern 공통 | sender/receiver 전용 `std::thread` | blocking raw send | 없음 | `POLLIN`, `wait(-1)`, DONTWAIT drain, wire STOP | recv/decode 처리 직후 monotonic `now_ns` | `recv_ts < active_deadline`; 이후 payload는 cleanup 소비 | 유효 active 수신 수 / 고정 duration |

모든 언어에서 latency와 throughput은 같은 유효 active payload 집합을 쓴다. one-way latency는 `recv_ts - sent_ts`이며 `/2.0` 보정은 세 언어 어디에도 없다.

### Go

| pattern | 진행 주체 | 동기/비동기 | yield | recv 모델 | timestamp | active 경계 | throughput 식 |
|---|---|---|---|---|---|---|---|
| PAIR | main sender + 고정 receiver goroutine | synchronous `Submit` | async/coroutine 없음 | POLLIN -1 + DONTWAIT drain | drain 직후 monotonic | recv 시각 `< StopAtNs` | valid count / duration |
| PUBSUB | main publisher + 고정 subscriber goroutine | synchronous publish `Submit` | 동일 | 동일 | 동일 | 동일 | 동일 |
| DEALER_DEALER | main sender + 고정 receiver goroutine | synchronous `Submit` | 동일 | 동일 | 동일 | 동일 | 동일 |
| DEALER_ROUTER | 고정 sender goroutine + 고정 main receiver | synchronous routed `Submit` | 동일 | 동일 | 동일 | 동일 | 동일 |
| ROUTER_ROUTER | 고정 sender goroutine + 고정 main receiver | synchronous routed `Submit` | 동일 | 동일 | 동일 | 동일 | 동일 |

- `bindings/go/perf/single/perf_oneway.go:52-76`, `perf_pubsub.go:53-77`, `perf_routed_oneway.go:76-99`를 poller readiness 뒤 DONTWAIT drain으로 변경했다.
- `perf_main.go:29`와 역할 goroutine의 `runtime.LockOSThread()`를 확인했다. timestamp, active membership, `Stats.Snapshot(duration, size)`는 기존부터 C와 같아 유지했다.
- 결과: C `perf_c_single_linux_20260908_165717_sgaudit-go2.txt`, Go `perf_go_single_linux_20260908_165955_sgaudit-go2.txt`; 같은 tag와 같은 티켓에서 C 먼저 실행했으며 각각 30/30 cells, fail 0, complete.

| pattern | Go sg1 64 B | 감사 후 64 B | 변화 |
|---|---:|---:|---:|
| PAIR | 637,364.8 | 651,921.6 | +2.3% |
| PUBSUB | 657,009.2 | 674,197.6 | +2.6% |
| DEALER_DEALER | 589,759.4 | 600,250.2 | +1.8% |
| DEALER_ROUTER | 593,455.6 | 582,834.0 | -1.8% |
| ROUTER_ROUTER | 539,193.2 | 548,644.0 | +1.8% |

### Rust

| pattern | 진행 주체 | 동기/비동기 | yield | recv 모델 | timestamp | active 경계 | throughput 식 |
|---|---|---|---|---|---|---|---|
| PAIR | sender `std::thread` + main receiver | awaitable을 sender thread가 직접 poll | executor/yield 없음 | POLLIN -1 + DONTWAIT drain | `handle_recv` monotonic | `recv_ts < active_deadline` | valid count / duration |
| PUBSUB | publisher `std::thread` + main subscriber | synchronous publish submit | 동일 | 동일 | 동일 | 동일 | 동일 |
| DEALER_DEALER | sender `std::thread` + main receiver | awaitable 직접 poll | 동일 | 동일 | 동일 | 동일 | 동일 |
| DEALER_ROUTER | dealer `std::thread` + main router | awaitable 직접 poll | 동일 | 동일 | 동일 | 동일 | 동일 |
| ROUTER_ROUTER | sender `std::thread` + main receiver | awaitable 직접 poll | 동일 | 동일 | 동일 | 동일 | 동일 |

- `bindings/rust/perf/single/src/one_way.rs:1-56`에 5 pattern 공용 poller/drain과 exact wire-shape 분류를 뒀다. 금지된 `common.rs`는 수정하지 않았다.
- 각 one-way 파일에서 deadline/1 ms idle 전환을 제거했다. routed 두 pattern도 `first_part()` 우회를 없애 공식 2-part `[payload, empty]`를 같은 helper로 검사한다.
- 첫 재검증에서 PAIR가 timeout됐다. 원인은 측정 macro가 STOP에도 empty tail을 붙인 것과, sender socket이 thread 종료 시 drop되어 accepted STOP 전달과 close가 경합한 것이다. STOP은 single-part submit으로 분리했고, 각 sender thread가 socket을 반환하여 receiver가 STOP을 본 뒤 join에서 회수한다(`perf_pair.rs:55-91`, `perf_pubsub.rs:60-105`, `perf_dealer_dealer.rs:57-93`, `perf_dealer_router.rs:88-122`, `perf_router_router.rs:106-140`). 임시 진단 출력은 제거했다.
- 64 B socket lifetime 확인 티켓은 rc=0. 최종 결과: C `perf_c_single_linux_20260908_164613_sgaudit-rust3.txt`, Rust `perf_rust_single_linux_20260908_164851_sgaudit-rust3.txt`; 각각 expected/actual result 150/150, complete.

| pattern | Rust before 64 B | 감사 후 64 B | 변화 |
|---|---:|---:|---:|
| PAIR | 1,530,043.4 | 1,536,218.8 | +0.4% |
| PUBSUB | 1,176,457.2 | 1,215,167.2 | +3.3% |
| DEALER_DEALER | 1,373,800.8 | 1,380,248.6 | +0.5% |
| DEALER_ROUTER | 1,386,539.6 | 1,396,330.4 | +0.7% |
| ROUTER_ROUTER | 1,174,671.0 | 1,195,512.0 | +1.8% |

### Python

| pattern | 진행 주체 | 동기/비동기 | yield | recv 모델 | timestamp | active 경계 | throughput 식 |
|---|---|---|---|---|---|---|---|
| PAIR | sender `threading.Thread` + main receiver | `submit_sync()` | coroutine/event-loop 없음 | POLLIN -1 + DONTWAIT drain | header 처리 직후 `monotonic_ns()` | `recv_ts < active_end_ns` | valid count / duration |
| PUBSUB | publisher `threading.Thread` + main subscriber | synchronous `submit()` | 동일 | 동일 | 동일 | 동일 | 동일 |
| DEALER_DEALER | sender `threading.Thread` + main receiver | `submit_sync()` | 동일 | 동일 | 동일 | 동일 | 동일 |
| DEALER_ROUTER | dealer `threading.Thread` + main router | `submit_sync()` | 동일 | 동일 | 동일 | 동일 | 동일 |
| ROUTER_ROUTER | sender `threading.Thread` + main receiver | routed `submit_sync()` | 동일 | 동일 | 동일 | 동일 | 동일 |

- `bindings/python/perf/single/perf_common.py:285-360`의 50 ms poll/stop deadline을 제거하고 C와 같은 무한 readiness wait + DONTWAIT drain으로 바꿨다. malformed multipart는 크기 불일치와 구분해 즉시 오류로 노출한다.
- 다섯 STOP helper가 재시도 소진 뒤 조용히 반환하던 경로를 명시적 `RuntimeError`로 바꿨다.
- 첫 재검증에서 ROUTER/ROUTER sender가 `submit_sync(flags=...)` TypeError로 종료되는 잠복 결함이 드러났다. 인자를 받지 않는 공개 terminal에 잘못 넘긴 `flags`를 제거했다(`perf_router_router.py:30-48`).
- 최종 결과: C `perf_c_single_linux_20260908_163456_sgaudit-python2.txt`, Python `perf_python_single_linux_20260908_164016_sgaudit-python2.txt`; 각각 30/30 cells, fail 0, complete.

| pattern | Python before 64 B | 감사 후 64 B | 변화 |
|---|---:|---:|---:|
| PAIR | 189,677.2 | 203,768.6 | +7.4% |
| PUBSUB | 180,518.8 | 192,925.8 | +6.9% |
| DEALER_DEALER | 175,960.0 | 199,398.2 | +13.3% |
| DEALER_ROUTER | 151,654.8 | 183,149.4 | +20.8% |
| ROUTER_ROUTER | 156,991.8 | 171,071.4 | +9.0% |

수정 전 Rust/Python 기준 report는 각각 `perf_rust_single_linux_20260908_154204_sgaudit-rust-before.txt`, `perf_python_single_linux_20260908_155415_sgaudit-python-before.txt`이며 둘 다 30/30 complete다.

수정 전에는 언어마다 recv 규칙이 3개였고(Go blocking-first, Rust deadline mode switch, Python 50 ms deadline poll), 수정 후에는 세 언어 모두 C의 poller/drain/STOP 규칙 하나다. Rust sender socket 수명도 “sender thread 종료”가 아니라 “receiver STOP 관측” 하나로 맞췄다.

## C. C Single DEALER_ROUTER_REQREP tcp 64 B

### 기존 증거와 보강

- 기존 `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_130119_sg1.txt`는 `non_zero_exit_1`만 남기고 case stderr를 버렸다. 이 report의 runtime은 `libzlink.so.0.17.2`였으므로 이번 고정 Core 0.17.3 결과와 직접 같은 후보로 단정할 수 없다.
- `bindings/c/perf/single/run_comparison.py:785-791,1253-1255,1411-1417,1514-1524`가 실패한 pattern/transport/size/run의 stderr를 `## Failure case logs`에 보존하도록 했다.
- `bindings/c/perf/single/src/perf_dealer_router_reqrep.cpp:134-157`은 종료 실패 때 debug flag 없이 requester fatal/in-flight/retained/wait-token/retry-ready와 replier received/replied/completed를 출력한다. 측정 hot path 밖의 실패 분기다.
- timeout, retry budget, HWM, active window, request admission/completion 모델은 변경하지 않았다.

### 5-run 재현

report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_154138_cdrr64.txt`

| run | throughput | mean latency |
|---:|---:|---:|
| 1 | 145,740 ops/s | 10.329 ms |
| 2 | 792,350 ops/s | 6.311 ms |
| 3 | 782,710 ops/s | 5.713 ms |
| 4 | 777,860 ops/s | 5.242 ms |
| 5 | 456,100 ops/s | 5.289 ms |
| median | 777,862.6 ops/s | 5.713 ms |

5/5 성공, fail 0, status complete다. 따라서 이번 범위에서는 실행 로직을 추측해 바꾸지 않았다. 다음 간헐 실패부터 동일 report에 정확한 case stderr와 종료 상태가 남는다.

## 변경 파일과 검증

- Node: `bindings/node/perf/multi/perf_multi_routed_sendsend.ts`, 대응 contract test와 생성된 `dist-tools` JS 2개.
- Go: `bindings/go/perf/single/perf_oneway.go`, `perf_pubsub.go`, `perf_routed_oneway.go`.
- Rust: one-way `perf_*.rs` 5개와 새 `src/one_way.rs`. `common.rs`와 REQREP 파일은 건드리지 않았다.
- Python: `perf_common.py`와 one-way pattern 파일 5개. REQREP 파일은 건드리지 않았다.
- C: `run_comparison.py`, `tests/test_run_comparison_policy.py`, `src/perf_dealer_router_reqrep.cpp`.

검증 결과:

- Node incremental build 및 관련 contract test 12/12 통과.
- Go `go vet ./perf/single`, `go test ./perf/single ./perf/internal/perfcommon` 통과; 고정 Core runner binary 재빌드 통과.
- Rust perf crate `cargo test` 14/14 통과, release 재빌드 통과. 제외 대상으로 지정된 binding test는 이 perf crate 실행에 포함되지 않았다.
- Python `py_compile` 통과, `test_perf_runner`와 `test_perf_monitor_hwm` 2/2 통과. 환경에 `pytest` module이 없어 pytest invocation 자체는 실행할 수 없었다. 잘못된 build-lib `PYTHONPATH`로 돌린 전체 perf unittest에서는 작업 범위 밖 multi 기본값 assertion 2건과 namespace import 오류 8건이 분리됐다.
- C report policy unittest 18/18 통과, `perf_dealer_router_reqrep` 빌드 통과.
- `git diff --check` 통과.

정책·스펙·계획서, Core, `framework/**`, binding library `bindings/*/src`, 동시 작업자가 지정한 REQREP 파일은 수정하지 않았다. commit/push도 수행하지 않았다.
