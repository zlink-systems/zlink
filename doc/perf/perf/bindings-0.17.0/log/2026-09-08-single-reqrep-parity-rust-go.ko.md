# Single REQREP 러너 D-BP40 정합 — Rust / Go (2026-09-08)

대상 파일: `bindings/rust/perf/single/**`, `bindings/go/perf/single/**`
기준 구현: `bindings/c/perf/single/common/perf_single_reqrep.hpp:396-443` (`run_request_phase`)
기준 정책: `doc/perf/PERF_SINGLE_TEST_POLICY.md` §1.1.3, §1.1.4 / 결정 `D-BP40`
참고 구현: C++ `bindings/cpp/perf/single/common/perf_single_reqrep.hpp`(`0ef05965fb`),
Node `bindings/node/perf/single/perf_socket_reqrep.ts`, .NET `bindings/dotnet/.../PerfReqRep.cs`(작업 트리)

**결론 요약**

- Rust는 turn당 1건 제출 뒤 completion을 블로킹 대기하는 구조여서 실효 미완료 깊이가 **2.0**이었고,
  C 대비 처리량이 **6.1~6.4 %**였다.
- Go는 미완료 상한이 없어 큰 크기에서 깊이가 **77~97**까지 부풀었고(C는 1.9), 그 결과
  65536 B 이상에서 latency가 C의 **146~169배**였다. 처리량은 C 대비 **16.6~18.8 %**.
- **두 언어 모두 latency를 `/2`로 반토막** 내고 있었다. 정책 §1.1.4는 "request 제출 시각부터 해당
  reply completion까지"(왕복 전체)이고 C도 그렇게 기록한다(`perf_single_reqrep.hpp:192-198`).
  즉 기존 Rust·Go의 latency 값은 정의 자체가 달랐다.
- D-BP40의 admission 창(Core가 requester 소켓에 적용한 SNDHWM bytes ÷ wire size)으로 미완료를
  묶고 C의 2단 turn 구조로 바꾼 뒤: **Rust DR 6.4 %→54.2 %, RR 6.1 %→55.1 %**,
  **Go DR 18.8 %→56.4 %, RR 16.6 %→55.2 %**. PAIR 64 B 회귀 없음.
- 고정 숫자 상한·env·timeout·sleep을 추가하지 않았다. 창은 monitor snapshot의
  `auto_hwm_applied_sndhwm_bytes`에서 읽고, 0이면 socket option SNDHWM, 둘 다 0이면 실패한다.

---

## 1. 차이 표

| 항목 | C (기준) | Rust (before) | Go (before) | Rust·Go (after) |
|---|---|---|---|---|
| 제출 gate | `zlink_request_part(DONTWAIT, FINAL)`이 `ZLINK_SUBMIT_BACKPRESSURED`를 낼 때까지 연속 제출 (`:418-438`) | turn당 **1건** (`common.rs:779-812`) | turn당 **1건** (`perf_reqrep.go:120-124`) | `outstanding < admission_window` 동안 **연속 제출** |
| 미완료 상한 | 없음(HWM이 경계) | 없음(코드상). 실질 경계는 turn당 블로킹 대기 | 없음(코드상·실질 모두) | **applied SNDHWM bytes ÷ wire size** (64 B→16384, 262144 B→4) |
| 제출 중 progress | 제출 64건마다 `poll(0)` | 없음 | 없음 | 제출 64건마다 non-blocking progress |
| 창 포화 시 | `poll(50 ms)` 블로킹 | `poller.wait(remaining)` — **남은 active 구간 전체**를 대기 | `poll(min(50 ms, 남은 구간))` | `poll(50 ms)` 블로킹 |
| reply drain | `zlink_completion_recv(DONTWAIT)`를 `NO_DATA`까지 | 미완료 Vec 전체를 `Future::poll` | `completed` 채널(용량 1) select drain | 완료되는 것부터 drain(Rust: Vec poll, Go: 채널 drain) |
| timestamp | 제출 직전 `stamp_payload(now_ns())` | 제출 직전 `encode_header`(동일) | 제출 goroutine 안에서 `NewWindowMessage`(동일) | 동일(변경 없음) |
| **latency 정의** | 왕복 전체 `now_ns - sent_ts_ns` (`:192-198`) | **왕복÷2** (`common.rs:745`) | **왕복÷2** (`perf_reqrep.go:88`) | **왕복 전체** (C와 동일) |
| 진행 주체 | 전용 requester thread | 전용 thread가 직접 `poll` | LockOSThread된 벤치 goroutine이 poller 소유 | 동일(§1.1.5 만족) |

## 2. 원인 (파일:줄, 수정 전 기준)

1. **Rust — 1건 제출 뒤 무한정 블로킹**: `bindings/rust/perf/single/src/common.rs:779-812`.
   active 루프가 `requests.push(submit(payload, request_timeout))` **1건**을 넣고 곧바로
   전체 Vec를 `poll`한 뒤, 아무것도 완료되지 않았으면
   `poller.wait(&mut events, remaining)`으로 **남은 active 구간 전부**를 블로킹했다.
   결과적으로 "한 건 보내고 응답을 기다린 뒤 다음을 보낸다"가 되어 실효 깊이가 2.0으로 고정된다
   (throughput × latency로 확인, 아래 4절).
2. **Go — 상한 없는 pipelining**: `bindings/go/perf/single/perf_reqrep.go:120-124`.
   turn당 1건을 goroutine으로 띄우고 `waitForProgress`가 최대 50 ms만 기다렸다가 다음 turn으로
   넘어간다. 완료를 기다리지 않으므로 미완료가 계속 쌓이고, 상한이 없어서 65536 B 이상에서
   깊이가 77~97까지 갔다(C는 1.9). Core가 wire를 HWM에서 막고 있는 동안 binding이 retain한
   요청만 늘어나는, 정책 §1.1.3이 경고한 상태다.
3. **latency 반토막(양쪽)**: `common.rs:745` `stats.record_ns(latency_ns / 2)`,
   `perf_reqrep.go:88` `stats.AddLatencySampleNs(float64(done.completedNs-sent) / 2.0)`.
   one-way 패턴의 편도 관례를 request-reply에 그대로 쓴 것으로 보이며, §1.1.4와 C 기준
   (`perf_single_reqrep.hpp:192-198`, 나눗셈 없음)에 어긋난다.
4. **Go — 요청 goroutine의 `runtime.LockOSThread()`**: `perf_reqrep.go:57`(수정 전).
   창을 적용하면 작은 크기에서 미완료가 수천~1만 6천 건이 되는데, 각 요청 goroutine이 OS thread를
   고정한 채 binding의 completion 채널에서 park하면 미완료 1건당 OS thread 1개가 묶여
   Go 런타임의 thread 한도(10000)를 넘는다. 정책 §1.1.4가 고정을 요구하는 것은 **역할** goroutine
   (요청 제출·완료 진행을 소유한 벤치 goroutine, replier goroutine)이고, 요청 1건짜리 goroutine은
   역할이 아니므로 고정을 제거했다(둘 다 여전히 `LockOSThread`).

## 3. diff 요지

공통(양쪽 동일한 모양, C++ `0ef05965fb`·Node 구현과 같은 형태):

```
admission_window = applied_sndhwm_bytes / wire_size      # 0이면 socket option SNDHWM, 둘 다 0이면 실패
                                                          # 최소 1건은 항상 허용
turn:
  while (deadline 전) and (outstanding < admission_window):
      submit()                       # awaitable/blocking terminal, 응답을 기다리지 않는다
      if ++submitted % 64 == 0: progress(0); drain()
  progress(50 ms); drain()           # 창이 찼을 때만 블로킹
```

- `bindings/rust/perf/single/src/common.rs`
  - `reqrep_admission_window(applied_sndhwm_bytes, option_sndhwm_bytes, wire_size)` 추가.
  - `drain_reqrep_completions()` 추가(미완료 Vec 1회 순회 poll → 완료분 집계·제거).
  - `run_reqrep(..., admission_window: usize, ...)` 시그니처에 창 추가, 위 turn 구조로 교체.
    `RequestOp::submit`의 Future는 **lazy**라 첫 `poll`에서 admission이 일어난다 — 그래서
    제출 직후 한 번 `poll`하고 `Pending`일 때만 미완료 집합에 넣는다(C가
    `zlink_request_part(DONTWAIT)`를 호출하는 지점과 같다).
  - `stats.record_ns(latency_ns / 2)` → `stats.record_ns(latency_ns)`.
- `bindings/rust/perf/single/src/perf_dealer_router_reqrep.rs`, `perf_router_router_reqrep.rs`
  - 이미 열려 있던 `requester_monitor.status().auto_hwm_applied_sndhwm_bytes`와
    `requester.common_options().send_high_water_mark()`로 창을 계산해 `run_reqrep`에 전달.
- `bindings/go/perf/single/perf_reqrep.go`
  - `reqRepAdmissionWindow(requester, monitor, wireSize)` 추가(같은 규칙, 둘 다 0이면 `Must(err)`).
  - `runSingleReqRep(..., requesterMon *zlink.SocketMonitor, ...)`로 monitor를 받는다.
  - `completed` 채널 용량 1 → `admissionWindow`(제출 goroutine이 hand-off에서 막히지 않게).
  - `waitForProgress(deadline, progressed)` → `progressOnce(wait)` (C의 `poll(0)`/`poll(50)`).
  - `drainSettled()` 추가: 요청 goroutine이 아직 스케줄되지 않아 완료가 안 보이는 구간을
    `runtime.Gosched()` **한 번**으로 넘긴다(sleep·timer 아님. Node 참고 구현의
    `await sleepImmediate()`와 같은 역할).
  - 요청 goroutine의 `runtime.LockOSThread()`/`UnlockOSThread()` 제거(2절 4번).
  - `/ 2.0` 제거.
- `bindings/go/perf/single/perf_dealer_router_reqrep.go`, `perf_router_router_reqrep.go`
  - `runSingleReqRep`에 `requesterMon` 전달(1줄씩).

binding 라이브러리·Multi·정책 문서는 건드리지 않았다.

## 4. 깊이 표와 before/after

깊이 = throughput × latency(초). before의 Rust·Go latency는 `/2`가 걸려 있었으므로 **×2**해서 계산했다.
창(window) = 적용 SNDHWM 1,048,576 B ÷ wire size.

### Rust — DEALER_ROUTER_REQREP (tcp, 5 s, 1-run)

| size | before tp | before/C | before 깊이 | after tp | C tp | after/C | after 깊이 | C 깊이 | 창 | lat after/C |
|---|---|---|---|---|---|---|---|---|---|---|
| 64 | 13,813 | 1.8 % | 2.0 | 507,081 | 807,675 | **62.8 %** | 96.4 | 5,934 | 16384 | 0.03 |
| 256 | 13,807 | 1.7 % | 2.0 | 469,986 | (C 셀 실패) | - | 104.5 | - | 4096 | - |
| 1024 | 13,637 | 1.9 % | 2.0 | 420,437 | 721,773 | **58.3 %** | 99.4 | 1,054 | 1024 | 0.16 |
| 65536 | 2,439 | 21.9 % | 1.0 | 4,825 | 10,882 | **44.3 %** | 1.91 | 1.94 | 16 | 2.22 |
| 131072 | - | - | - | 4,589 | 8,622 | **53.2 %** | 1.94 | 1.94 | 8 | 1.88 |
| 262144 | - | - | - | 3,523 | 6,701 | **52.6 %** | 1.97 | 1.93 | 4 | 1.94 |

평균: before **6.4 %**(64~65536, 4096 포함 5셀) → after **54.2 %**(5셀).

### Rust — ROUTER_ROUTER_REQREP

| size | before tp | before/C | before 깊이 | after tp | C tp | after/C | after 깊이 | C 깊이 | 창 | lat after/C |
|---|---|---|---|---|---|---|---|---|---|---|
| 64 | 13,887 | 1.6 % | 2.0 | 511,680 | 883,572 | **57.9 %** | 105.6 | 139.0 | 16384 | 1.31 |
| 256 | 13,734 | 1.7 % | 2.0 | 469,619 | 824,714 | **56.9 %** | 110.8 | 267.1 | 4096 | 0.73 |
| 1024 | 13,609 | 2.0 % | 2.0 | 433,019 | 693,768 | **62.4 %** | 100.6 | 5,160 | 1024 | 0.03 |
| 65536 | 2,245 | 20.1 % | 1.0 | 5,346 | 10,692 | **50.0 %** | 1.90 | 1.94 | 16 | 1.96 |
| 131072 | - | - | - | 4,550 | 9,037 | **50.4 %** | 1.95 | 1.94 | 8 | 1.99 |
| 262144 | - | - | - | 3,420 | 6,434 | **53.2 %** | 1.97 | 1.93 | 4 | 1.92 |

평균: before **6.1 %** → after **55.1 %**.

### Go — DEALER_ROUTER_REQREP

| size | before tp | before/C | before 깊이 | after tp | C tp | after/C | after 깊이 | C 깊이 | 창 | lat after/C |
|---|---|---|---|---|---|---|---|---|---|---|
| 64 | 31,288 | (C 셀 실패) | 7.4 | 306,881 | 776,695 | **39.5 %** | 240.1 | 3,522 | 16384 | 0.17 |
| 256 | 32,115 | 3.9 % | 7.9 | 311,270 | 837,152 | **37.2 %** | 236.0 | 412.3 | 4096 | 1.54 |
| 1024 | 32,798 | 4.4 % | 8.0 | 317,376 | 710,873 | **44.6 %** | 230.7 | 956.0 | 1024 | 0.54 |
| 65536 | 3,212 | 28.1 % | 86.1 | 7,245 | 10,522 | **68.9 %** | 15.77 | 1.94 | 16 | 11.80 |
| 131072 | 2,502 | 27.1 % | 76.6 | 6,266 | 8,525 | **73.5 %** | 7.83 | 1.94 | 8 | 5.50 |
| 262144 | 2,230 | 30.7 % | 89.6 | 5,195 | 6,974 | **74.5 %** | 3.83 | 1.93 | 4 | 2.67 |

평균: before **18.8 %**(5셀) → after **56.4 %**(6셀).

### Go — ROUTER_ROUTER_REQREP

| size | before tp | before/C | before 깊이 | after tp | C tp | after/C | after 깊이 | C 깊이 | 창 | lat after/C |
|---|---|---|---|---|---|---|---|---|---|---|
| 64 | 32,090 | 3.6 % | 6.1 | 326,277 | 886,737 | **36.8 %** | 223.4 | 130.4 | 16384 | 4.66 |
| 256 | 32,943 | 3.9 % | 6.2 | 329,225 | 808,926 | **40.7 %** | 225.1 | 207.4 | 4096 | 2.67 |
| 1024 | 32,358 | 4.5 % | 6.4 | 325,630 | 689,363 | **47.2 %** | 237.0 | 4,882 | 1024 | 0.10 |
| 65536 | 3,096 | 27.5 % | 83.6 | 6,726 | 10,892 | **61.8 %** | 15.76 | 1.94 | 16 | 13.14 |
| 131072 | 2,708 | 29.6 % | 96.7 | 6,446 | 8,741 | **73.7 %** | 7.84 | 1.94 | 8 | 5.48 |
| 262144 | 2,138 | 30.4 % | 93.4 | 5,045 | 7,097 | **71.1 %** | 3.84 | 1.93 | 4 | 2.79 |

평균: before **16.6 %** → after **55.2 %**.

### 회귀 (PAIR tcp 64 B, 1-run)

| 언어 | before | after | 변화 |
|---|---|---|---|
| Rust | 1,489,326 msg/s | 1,509,529 msg/s | +1.4 % |
| Go | 637,365 msg/s | 644,440 msg/s | +1.1 % |

### report 경로

- Rust after: `bindings/rust/perf/results/single/report/perf_rust_single_linux_20260908_161653_sgfix-rust.txt`
- Rust 짝 C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_153813_sgfix-rust.txt`
  (`DEALER_ROUTER_REQREP tcp 256B`가 `non_zero_exit_1`로 실패 — C 러너 쪽 shutdown 결함이며
  `replier_fatal=1 received=3270498 replied=0`. 이 티켓은 `set -e` 때문에 Rust leg까지 중단돼
  Rust만 같은 태그로 재제출했다.)
- Rust before: `bindings/rust/perf/results/single/report/perf_rust_single_linux_20260908_134416_rust_costmap_single_before_rust.txt`
  / 짝 C `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_134325_rust_costmap_single_before_c.txt`
- Go after: `bindings/go/perf/results/single/report/perf_go_single_linux_20260908_154025_sgfix-go.txt`
  / 짝 C `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_153924_sgfix-go.txt`
- Go before: `bindings/go/perf/results/single/report/perf_go_single_linux_20260908_135357_sg1.txt`
  / 짝 C `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_130119_sg1.txt`
- 회귀: `bindings/rust/perf/results/single/report/perf_rust_single_linux_20260908_154127_sgfix-reg-rust.txt`,
  `bindings/go/perf/results/single/report/perf_go_single_linux_20260908_154132_sgfix-reg-go.txt`
  (Rust PAIR before는 같은 코드가 도는 `perf_rust_single_linux_20260908_150719_a173smoke3all.txt`)
- 참고: 감독자 prio 0 티켓 `perf_rust_single_linux_20260908_150719_a173smoke3all.txt`(1 s smoke)가
  수정된 Rust 러너로 전 pattern을 돌려 crash 없이 통과했다(210/210 result line).

## 5. binding 한계로 본 것 (수정하지 않은 것)

1. **창 모델과 C의 실제 깊이 차이 — Go 큰 크기**. 65536/131072/262144 B에서 창은 16/8/4인데
   C의 실제 깊이는 1.93~1.94다. Go는 창을 그대로 채우므로(깊이 15.8/7.8/3.8) latency가 C의
   11.8x/5.5x/2.7x가 된다. 처리량은 오히려 C의 62~75 %로 가장 좋다. C는
   `ZLINK_SUBMIT_BACKPRESSURED`라는 **실제 admission 거절**을 보고 멈추는데, Go의 blocking
   `Submit(ctx)`은 그 신호를 돌려주지 않아 러너가 볼 수 없다. D-BP40의 창은 그 경계의 상계이지
   같은 값이 아니다. C++ 커밋 `0ef05965fb`가 기록한 것과 같은 한계다.
2. **Rust 큰 크기는 창이 아니라 per-message 비용이 경계**. Rust는 65536 B 이상에서 깊이가
   1.90~1.97로 **C와 같은데** 처리량이 44~53 %다(= latency가 1.9~2.2x). 창이 원인이 아니므로
   이 격차는 러너가 아니라 Rust binding의 요청 경로 비용이다. 이 과제 범위 밖이라 손대지 않았다.
3. **작은 크기 C latency의 양안정**. C의 DR 64 B latency가 같은 날 4.5 ms(깊이 3,522)와
   0.16 ms 사이를 오가고 RR 1024 B는 7.1 ms(깊이 4,882)다. 크기별 latency 비율(`lat after/C`)이
   0.03~4.7로 흩어지는 것은 대부분 C 쪽 셀의 이 양안정 때문이며, 러너 결함이 아니다
   (`log/2026-09-08-single-reqrep-parity-cpp-java.ko.md` §4와 같은 현상).
4. **Go 요청 goroutine을 `LockOSThread`로 고정할 수 없다**(2절 4번). 정책 §1.1.4의 문언은
   "역할별 goroutine"이므로 위반은 아니라고 판단했지만, 감독자가 다르게 읽는다면
   §1.1.4에 한 줄 명문화가 필요하다.
5. **C 러너 DR 256 B 셀의 shutdown 실패**는 이 과제 범위 밖(C 러너)이라 그대로 두고 보고만 한다.
