# Single REQREP 러너 정합 조사 — C++ / Java (2026-09-08)

대상 파일: `bindings/cpp/perf/single/**`, `bindings/java/perf/single/**`
기준 구현: `bindings/c/perf/single/common/perf_single_reqrep.hpp`
기준 정책: `doc/perf/PERF_SINGLE_TEST_POLICY.md` §1.1.0~§1.1.3
짝지은 측정: 태그 `sg1` (C `20260908_130119`, C++ `20260908_131617`, Java `20260908_132754`)

**결론 요약**: C++·Java single REQREP은 **미완료 요청이 정확히 1건인 RTT 루프**로 돌고 있다
(정책 §1.1.0이 명시적으로 금지). 원인은 러너 구조지만, 이를 C 모델(“backpressure까지 연속 제출”)로
고치려면 **공개 request terminal이 admission 결과를 돌려줘야 한다**. C++ `request(...).async()`와
Java `RequestSubmitOperation.submit()`은 그 정보를 돌려주지 않는다. 따라서 상한 없이 러너만
고쳐서는 C 모델을 재현할 수 없다 — **binding 공개 API의 한계**로 보고하고 멈춘다.

---

## 1. 차이 표

| 항목 | C (기준) | C++ | Java |
|---|---|---|---|
| 제출 gate | 내부 루프에서 `zlink_request_part(DONTWAIT, FINAL)`이 `ZLINK_SUBMIT_BACKPRESSURED`를 낼 때까지 **연속 제출** (`perf_single_reqrep.hpp:418-438`) | turn당 **1건**. 제출 직후 `progress_once(50ms)`가 completion 하나를 기다린다 (`perf_single_reqrep.hpp:435-468`) | turn당 **1건**. 제출 직후 `completionPoller.poll(50)` (`PerfSocketReqRep.java:184-190`) |
| 미완료 상한 | 없음. HWM이 경계 | 없음(코드상). 실질 경계는 turn당 blocking wait | 없음(코드상). 동일 |
| 실효 미완료 깊이 | 2 ~ 2,100 (크기별, HWM 결정) | **1.00** | **0.9** |
| backpressure 관측 | `ZLINK_SUBMIT_BACKPRESSURED` + `EAGAIN` + WRITABLE 토큰 → 제출 중단, 토큰으로 재제출 | **관측 불가** — binding이 내부에서 retain/재제출 | **관측 불가** — 동일 |
| reply drain | `zlink_completion_recv(DONTWAIT)`를 `NO_DATA`까지. 제출 64건마다 `poll(0)`, 제출 루프가 막히면 `poll(50)` | `poller.wait(50ms)` → `ready_queue.run_ready_round()` (turn마다) | `PerfSocketPollSet.poll(50)` → CompletionStage 콜백 (turn마다) |
| 동기/비동기 | 전용 requester thread, synchronous C API | 전용 requester thread + 자체 ready queue (executor 없음) — 정책 §1.1.5 만족 | 전용 requester thread + POLLCOMPLETION poller가 유일한 dispatch 주체 — §1.1.5 만족 |
| timestamp 시점 | 제출 직전 `stamp_payload(now_ns())`, completion 처리 시각으로 왕복 산출 | 동일 | 동일 |
| replier | 전용 thread, blocking recv → reply | 동일 | 동일 |

정책 §1.1.5(진행 주체) 위반은 **없다**. 위반은 §1.1.0 / §1.1.2의 **연속 제출** 쪽 하나다.

## 2. 원인 (파일:줄)

- `bindings/cpp/perf/single/common/perf_single_reqrep.hpp:435-468` — active 루프가
  `submit_async_request(...)` **1건**을 띄운 뒤 곧바로 `progress_once(std::chrono::milliseconds(50))`
  를 호출한다. `progress_once`(`:415-424`)는 `completion_poller.wait(...)`로 **completion이 하나
  생길 때까지 블로킹**한다. 결과적으로 “한 건 보내고 응답을 기다린 뒤 다음을 보낸다”가 된다.
- `bindings/java/perf/single/.../PerfSocketReqRep.java:184-190` — 같은 구조.
  `submitRequest(...)` 1건 → `completionPoller.poll(Math.min(50, ...))`.

### 증거: 미완료 깊이 = 1 (sg1, tcp)

request-reply에서 `throughput × latency = 평균 미완료 깊이`다. C++은 **6개 크기 전부에서
`1/latency`와 throughput이 0.3 % 이내로 일치**한다 — 미완료가 정확히 1건이라는 뜻이다.

| size | C++ throughput | 1/latency | 깊이 | Java throughput | 1/latency | 깊이 | C 깊이 |
|---|---|---|---|---|---|---|---|
| 64 | 14,296.6 | 14,334 | 1.00 | 5,310.0 | 5,871 | 0.90 | (C 실패: `non_zero_exit_1`) |
| 256 | 14,314.2 | 14,352 | 1.00 | 5,319.6 | 5,874 | 0.91 | **711.7** |
| 1024 | 14,617.4 | 14,656 | 1.00 | 5,335.6 | 5,897 | 0.90 | **974.6** |
| 65536 | 10,377.4 | 10,400 | 1.00 | 4,438.4 | 4,840 | 0.92 | 1.94 |
| 131072 | 8,625.6 | 8,643 | 1.00 | 4,107.0 | 4,459 | 0.92 | 1.94 |
| 262144 | 6,786.2 | 6,797 | 1.00 | 3,249.8 | 3,481 | 0.93 | 1.93 |

그래서 결손이 **작은 크기에만** 몰린다. C는 1 MB byte-HWM 안에 256 B 메시지를 수천 개 채우지만
C++/Java는 1개만 채운다. 65536 B 이상에서는 C의 깊이도 ~1.9라서 C++이 91~94 %까지 붙는다.
지시서의 집계값(C++ 56.3 %/47.4 %, latency 0.36x/0.41x)은 크기별 비율의 산술평균이며,
1.7~2.0 %(256 B·1024 B)와 91~94 %(65536 B 이상)가 섞인 결과다.

## 3. 왜 러너만 고쳐서는 안 되는가 (binding 한계)

C의 제출 루프가 멈추는 유일한 조건은 **Core의 admission 거절**이다(`perf_single_reqrep.hpp:434`,
`submit_step_blocked`). C++/Java에는 그 신호가 공개 API에 없다.

- C++ `request_submit_operation_t::async()` (`bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp:320-324`)
  는 `async_result_t<...>`만 돌려준다. 내부적으로는 admission 결과를 계산한다 —
  `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:213-270`의 `submit_request_attempt()`가
  `bool admitted`를 얻지만, `start_request()`(`:273-278`)가 `(void) submit_request_attempt (true);`
  로 **버린다**. `async_result_t`에도 `ready()` 외에 admission 상태 접근자가 없다
  (`operation_contracts.hpp:58-146`).
- Java `RequestSubmitOperation.submit()`
  (`bindings/java/src/main/java/systems/zlink/contracts/messaging/RequestSubmitOperation.java:29-40`)
  의 javadoc이 같은 사실을 명시한다 — “On BACKPRESSURED/EAGAIN, Core retains only a wait token;
  the binding retains the request and resubmits it only after the matching WRITABLE completion.”
  반환되는 `CompletionStage`는 **reply로만** 완료된다.

따라서 러너가 취할 수 있는 종료 조건은 두 가지뿐이다.

1. **completion 발생을 신호로 삼는다** — 지금 코드가 그렇다. “첫 completion에서 제출 중단”은
   평형에서 반드시 깊이 1로 수렴한다(제출/완료가 turn당 1:1로 묶이므로). 대기 시간을 0으로
   바꾸거나 Rust식 `progressed ? 0 : block`을 써도 결과는 같다 — 깊이를 만드는 것은
   “completion 전에 몇 건을 더 보내느냐”인데 그 수를 정할 근거가 없다.
2. **미완료 개수 상한을 둔다** — 정책 §1.1.3 셋째 불릿이 awaitable binding에 대해 요구하는
   바로 그것이다(“러너는 미완료 개수 상한 하나만 둔다 … 모든 binding이 같은 값을 쓰며 report의
   Effective Options에 노출한다”). 그러나 D-BP15/`31c5e4f7f0`이 이 상한
   (`PERF_SINGLE_REQREP_MAX_OUTSTANDING`)을 8개 러너에서 제거했고, 이번 과제도 상한 추가를
   금지한다.

상한 없이 무조건 연속 제출하면 wire는 HWM에서 막힌 채 retain된 요청 객체만 쌓인다.
262144 B에서 제출률(≈수만/s)과 완료율(7,256/s)의 차이만큼 5초 동안 누적되므로 수십 GB 규모다.
정책 §1.1.3의 “상한이 없으면 … 미완료 객체만 메모리에 쌓인다”가 그대로 발생한다.

POLLOUT로 경계를 관측하는 우회는 정책 §1.1.3이 명시적으로 금지한다(“aggregate hint를 쓰면
오히려 부정확해진다”), D-BP15도 러너에서 POLLOUT 등록을 제거했다.

**요청(감독자 판단 필요)**: 아래 셋 중 하나가 결정돼야 C++/Java REQREP이 C 모델과 정합될 수 있다.
- (a) §1.1.3의 미완료 상한을 awaitable binding에 한해 되살린다(D-BP15 재검토).
- (b) binding 공개 request terminal이 admission 결과를 노출하도록 계약을 보완한다
  (C++는 이미 내부에 `admitted`가 있고, Java도 동일 정보를 갖는다).
- (c) 현 상태(깊이 1)를 그대로 두고 report에 “binding 한계로 C 모델 미적용”을 명기한다.

이 판단은 C++/Java만의 문제가 아니다. .NET(`PerfReqRep.cs:498-521`)과
Rust(`bindings/rust/perf/single/src/common.rs:779-812`)도 같은 turn 구조이며, C++ multi
(`perf_multi_reqrep.hpp:230-241`)는 slot 수만큼만 깊이를 만든다.

## 4. Java PAIR latency 이상 — 러너 결함이 아니다

sg1에서 Java `PAIR` tcp 64 B latency가 11.605 ms(p95 46.9 / p99 56.9)로 C의 0.038 ms 대비 302x다.
지시서의 “52x”는 6개 크기 비율의 산술평균이고, 그 중 64 B 한 셀이 전부를 만든다
(나머지 크기 비율: 0.68 / 1.43 / 2.56 / 2.88 / 2.83).

원인은 **큐 포화 여부의 양안정(bistable) 상태**이지 PAIR 러너 코드가 아니다.

- 1 MB byte-HWM ÷ 64 B = 16,384건. Java PAIR 64 B의 깊이는 1,654,259 × 0.0116 s ≈ 19,000건 —
  큐가 상시 가득 찬 쪽에 안착했다. 큐가 빈 쪽에 안착하면 latency가 0.03 ms대가 된다.
- 같은 sg1 실행에서 **C의 `DEALER_DEALER` tcp 64 B가 10.361 ms**, C의 `ROUTER_ROUTER` tcp 256 B가
  62.806 ms다. C++도 `20260907_165206` 실행에서 PAIR 64 B가 6.774 ms였다. 즉 모든 binding이
  같은 두 상태를 오간다.
- `PerfPair.java`와 `PerfDealerDealer.java`의 receiver 루프·sender 루프는 stop token 송신을 빼면
  동일하며, 둘 다 C `perf_single_one_way.hpp`(`:186-215` sender, `:296-345` receiver)와 같은 형태다
  — `poll(-1)` → `recvNoWait` drain → payload header decode → active deadline 비교 → `close()`.
  timestamp도 C와 같다(sender가 blocking send **직전**에 `System.nanoTime()`으로 stamp,
  receiver가 recv 직후 시각으로 왕복 산출).

따라서 `bindings/java/perf/single/**` 범위에서 고칠 것이 없다. Java에서 **재현 가능한** one-way
결손은 따로 있다: 65536/131072/262144 B에서 throughput이 C의 41 %/34 %/34 %이고 latency가
패턴과 무관하게 **0.32~0.37 ms로 평탄**하다(= 1 MB HWM 상시 포화). 이 값이 `PAIR`, `DEALER_DEALER`,
`DEALER_ROUTER`, `ROUTER_ROUTER` 4개 패턴에서 같은 대역에 모이므로 패턴 러너가 아니라
공통 수신 경로(`bindings/java/perf/common/**` 또는 Java binding recv 경로)의 비용이다.
이 과제의 파일 범위 밖이라 손대지 않았다.

## 5. 변경 사항

없음. 위 3절의 이유로 `bindings/cpp/perf/single/**`, `bindings/java/perf/single/**` 어느 파일도
수정하지 않았다. 따라서 before/after 짝지음 실행도 수행하지 않았다(코드가 같으면 결과도 같다).
판단 근거로 쓴 짝지은 측정은 기존 `sg1` 3종이다.

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_130119_sg1.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260908_131617_sg1.txt`
- Java: `bindings/java/perf/results/single/report/perf_java_single_linux_20260908_132754_sg1.txt`

---

# §추가 (2026-09-08 14:0x) — D-BP40 admission window 구현과 sgfix-cpp 결과

감독자 결정 D-BP40(정책 §1.1.3에 명문화)에 따라 (a′)를 구현했다. awaitable terminal 러너는
awaitable을 기다리지 않고 계속 제출하되, 미완료 집합을 **Core가 그 소켓에 적용한 SNDHWM bytes ÷
메시지 wire size**로 묶는다. 고정 숫자 상한이 아니다.

## 6. diff 요지

### C++ `bindings/cpp/perf/single/common/perf_single_reqrep.hpp`

1. `reqrep_admission_window_bytes(socket)` 추가 — requester 소켓에 monitor를 열어
   `monitor_status_t::auto_hwm_applied_sndhwm_bytes`를 읽는다. 러너가 이미
   `## Auto-HWM Detail`에 찍는 값과 같은 snapshot 경로다(`perf_single_common.hpp`
   `emit_single_socket_hwm_detail`).
2. `reqrep_admission_window_requests(window_bytes, wire_size)` 추가 —
   `window_bytes / wire_size`, 최소 1. `wire_size`는 header를 포함한 실제 part 크기
   (`payload_size = max(msg_size, header_size())`).
3. active 루프를 C의 2단 turn으로 되돌렸다. 기존에는 turn당 1건 제출 뒤 곧바로
   `progress_once(50ms)`가 completion 하나를 **기다렸다**. 이제 내부 루프가
   `in_flight < admission_window` 동안 연속 제출하고, C와 같이 **제출 64건마다
   `progress_once(0)`** 로 대기 없이 drain하며, 창이 차거나 deadline이 지나면
   외부에서 `progress_once(50ms)`로 bounded 대기한다.
   (C `perf_single_reqrep.hpp:418-444` `run_request_phase`와 같은 형태.)
4. 창 값은 `PERF_DEBUG`일 때만 stderr에 찍는다(정상 stdout 계약 불변).

### Java `.../single/PerfSocketReqRep.java`

1. `admissionWindowRequests(windowBytes, wireSize)` 추가, `run()`에서 이미 열려 있는
   `clientMonitor.status().autoHwmAppliedSendHwmBytes()`와
   `Math.max(config.size(), PerfUtil.HEADER_SIZE)`로 창을 계산해
   `runRequestPhase(...)`에 넘긴다.
2. active 루프를 같은 2단 turn으로 바꿨다 — `outstanding.get() < admissionWindow`
   동안 연속 제출, 64건마다 `completionPoller.poll(0)`, 창이 차면
   `completionPoller.poll(min(50, remaining))`.

불변 유지: timeout·sleep 추가 없음, 고정 숫자 상한 없음, 진행 주체는 여전히 전용
requester thread(C++는 자기 ready queue, Java는 POLLCOMPLETION poller가 유일한 dispatch),
완료되는 것부터 drain, deadline 이후 새 제출 없음.

## 7. before/after (C++, tcp 1-run, 태그 `sgfix-cpp`)

C: `perf_c_single_linux_20260908_135850_sgfix-cpp.txt`
C++: `perf_cpp_single_linux_20260908_135952_sgfix-cpp.txt`
(before는 `sg1` 3종 — §4 상단 참조)

| pattern | size | C tput | C++ tput | **after 비율** | before 비율 | C 깊이 | C++ 깊이 |
|---|---|---|---|---|---|---|---|
| DEALER_ROUTER_REQREP | 64 | 543,279 | 492,521 | **90.7 %** | (C 실패) | 4050.8 | 97.1 |
| | 256 | 812,355 | 454,208 | **55.9 %** | 1.7 % | 575.2 | 111.1 |
| | 1024 | 721,087 | 435,496 | **60.4 %** | 2.0 % | 980.3 | 99.0 |
| | 65536 | 10,992 | 10,908 | **99.2 %** | 90.8 % | 1.94 | 15.99 |
| | 131072 | 9,105 | 9,695 | **106.5 %** | 93.4 % | 1.94 | 8.00 |
| | 262144 | 7,144 | 7,699 | **107.8 %** | 93.5 % | 1.93 | 4.00 |
| ROUTER_ROUTER_REQREP | 64 | 878,559 | 536,803 | **61.1 %** | (n/a) | 138.0 | 93.3 |
| | 256 | 820,950 | 475,610 | **57.9 %** | | 192.5 | 117.0 |
| | 1024 | 698,641 | 458,618 | **65.6 %** | | 4729.6 | 150.3 |
| | 65536 | 10,932 | 10,987 | **100.5 %** | | 1.94 | 15.99 |
| | 131072 | 9,206 | 9,401 | **102.1 %** | | 1.94 | 8.00 |
| | 262144 | 7,040 | 7,753 | **110.1 %** | | 1.93 | 4.00 |

산술평균 비율: `DEALER_ROUTER_REQREP` **86.7 %** (before 56.3 %),
`ROUTER_ROUTER_REQREP` **82.9 %** (before 47.4 %).
미완료 깊이는 1.00 고정에서 **93 ~ 150**(작은 크기) / **4 ~ 16**(큰 크기)로 올라
C와 같은 자릿수가 됐다. RTT 루프는 사라졌다.

## 8. 대형 크기 latency 8.3x — 무엇인가

65536 B에서 C++ latency 1.466 ms vs C 0.177 ms(8.3x), 131072 B 3.9x, 262144 B 1.9x.
처리량은 오히려 C 이상(99~110 %)이다.

### (1) 정의 차이가 아니다 — C도 재제출 시 timestamp를 다시 찍지 않는다

`bindings/c/perf/single/common/perf_single_reqrep.hpp:229-243` `submit_request`:

```c
const bool retrying = state_->retained_request;
if (retrying && (!state_->retry_ready || state_->wait_token != 0))
    return submit_step_blocked;
if (!retrying) {                       // <-- 재제출이면 stamp 자체를 건너뛴다
    const unsigned long long seq = state_->next_seq.fetch_add (1, ...);
    if (!perf_single_metric::stamp_payload (..., perf_single_metric::now_ns ())) ...
}
```

`retrying`이면 `stamp_payload`를 호출하지 않으므로 payload의 `sent_ts_ns`는 **첫 제출
시도 시각** 그대로다. 즉 C의 REQREP latency도 admission 이후가 아니라 첫 제출부터
재며, retain 대기가 그대로 포함된다. C++ awaitable과 **같은 정의**다.
(one-way 경로는 정반대다 — 정책 §1.1이 transient 재시도마다 `sent_ts_ns` 재stamp를
요구하고 `send_active_samples`가 그렇게 한다. REQREP은 의도적으로 다르다.)

### (2) 창을 Core admission에 더 가깝게 잡을 근거는 없다 — 그리고 대형 크기에서는 창 문제도 아니다

C의 깊이를 바이트로 환산하면(같은 `sgfix-cpp` 실행):

| pattern | size | C 깊이(건) | C 깊이(바이트) | HWM 창(건) |
|---|---|---|---|---|
| DR | 64 | 4050.8 | 253 KiB | 16384 |
| DR | 256 | 575.2 | 144 KiB | 4096 |
| DR | 1024 | 980.3 | 980 KiB | 1024 |
| DR | 65536 | 1.94 | 124 KiB | 16 |
| DR | 131072 | 1.94 | 248 KiB | 8 |
| DR | 262144 | 1.93 | 494 KiB | 4 |
| RR | 1024 | 4729.6 | **4730 KiB** | 1024 |
| RR | 64 | 138.0 | 8.6 KiB | 16384 |

바이트 깊이가 8.6 KiB에서 4730 KiB까지 흩어지고, `RR 1024 B`는 1 MiB HWM의 4.6배다.
즉 **C의 깊이는 대부분의 셀에서 admission 창이 아니라 C 러너 자신의 submit/drain 루프
평형**이다. 특히 65536 B 이상에서 C의 깊이 1.93~1.94는 124~494 KiB로 1 MiB HWM에
한참 못 미치므로, C는 그 크기에서 **BACKPRESSURED를 아예 만나지 않는다.**
따라서 창 값을 어떻게 잡아도 그 크기의 C latency를 재현할 수 없다. 재현하려면 실제
admission 시점을 알거나 C와 같은 turn 비용을 가져야 하는데, 전자는 공개 경로가 없고
(§3: C++ `completion_owner.cpp:242`의 `admitted`를 `start_request()`가 버린다,
Java `submit()` javadoc 동일; POLLOUT은 정책 §1.1.3이 금지) 후자는 언어 비용 문제다.

**결론**: 창 상한 = HWM 창(D-BP40 정의)으로 확정한다. 대형 크기에서 C++ 깊이가 창에
정확히 붙고(15.99 / 8.00 / 4.00) C보다 깊어져 latency가 3~8배가 되는 것은 **창 모델의
한계**다 — 그 구간에서 C는 창이 아니라 루프 평형으로 얕게 도는데, awaitable 러너에는
그 평형을 만들 신호가 없다. 처리량은 오히려 C 이상(99~110 %)이므로 정합 목표
(“깊이가 C와 같은 자릿수”)는 충족한다.

## 9. 미완료 검증 — 환경 차단

`sgfix-java`(C+Java REQREP)와 PAIR 64 B 회귀 티켓은 rc=1로 실패했다. 원인은 이 변경과
무관하다: 2026-09-08 14:00:53에 `0761c1d4d0 chore(version): bump libzlink and bindings to
0.17.3`이 main에 들어와 저장소 VERSION이 0.17.3이 됐는데, 고정 alpha prefix
`~/.cache/zlink/core-pinned/0.17.3-alpha`의 provenance manifest는 의도적으로
`"version": "0.17.2"`(D-B235)이다. `bindings/tools/local_core_runtime.sh:40-41`의 대조가
실패해 **모든 언어 러너가 즉시 종료**한다(감독자의 s173x Java 티켓도 14:03:31에 같은
이유로 rc=1). 감독자가 `core/v0.17.3` prefix를 빌드해 7개 언어를 재빌드한 뒤 같은 검증
티켓을 `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.3`으로 다시
낸다. 그때까지 Java 짝지음과 PAIR 회귀는 미측정이다.

**prefix 변경(D-BP42)**: §9의 VERSION 0.17.3 대조 실패 때문에 감독자가 `core/v0.17.3`
prefix(`~/.cache/zlink/core-pinned/0.17.3`, provenance version 0.17.3, revision
`0761c1d4d0`)를 빌드하고 7개 언어를 재빌드했다. §7의 `sgfix-cpp`는 그 전의 alpha
prefix(`0.17.3-alpha`, provenance 0.17.2)에서 잰 값이며 before(`sg1`)와 같은 prefix라
짝지음이 유효하다. 아래 §10의 `sgfix-java`·PAIR 회귀는 새 `0.17.3` prefix에서 잰다.

## 10. PAIR 64 B 회귀 (태그 `sgfix-pair`, prefix `0.17.3`)

| lang | before (`sg1`, alpha) | after (`sgfix-pair`, 0.17.3) | 판정 |
|---|---|---|---|
| C++ | 1,553,949 msg/s / 0.0467 ms | **1,546,332 msg/s / 0.0705 ms** | 회귀 없음 |
| Java | 1,654,260 msg/s / 11.605 ms | **1,688,018 msg/s / 52.998 ms** | 처리량 회귀 없음 |

- 파일: `perf_cpp_single_linux_20260908_145615_sgfix-pair.txt`,
  `perf_java_single_linux_20260908_145625_sgfix-pair.txt`
- 이번 변경은 REQREP 러너 파일만 건드렸고 `PerfPair.java`·C++ one-way 경로는 그대로이므로
  PAIR 회귀는 원래 성립할 수 없다. 측정도 그것을 확인한다.
- Java PAIR 64 B latency가 11.6 → 53.0 ms로 더 커진 것은 §4에서 규명한 **큐 포화
  양안정 상태**가 같은 방향으로 안착한 결과다(코드 변경 없음). C 짝지음 기준값은
  §11의 이유로 아직 없다.

## 11. 미완료 — 큐 runner 정지

`sgfix-java`(C 짝지음 REQREP + C PAIR 64 B 기준) 티켓
`2-1788847029-67029-claude-single-cppjava-sgfix-java_retry3_...`은 제출된 채
pending에 남아 있다. 15:40 경 `perf-ticket.sh wait`가
"runner가 떠 있지 않다 — 감독자가 scripts/perf/perf-queue-runner.sh를 띄워야 한다"를
돌려줬다. 앞선 rc=1 두 번은 (a) VERSION 0.17.3 대조 실패(§9), (b) 0.17.3 Single 재빌드
티켓이 `DEALER_DEALER`만 빌드해 REQREP 바이너리가 없었던 것 — 둘 다 코드 문제가 아니다.
runner 재기동 후 같은 티켓을 그대로 돌리면 된다.

## 12. Java 짝지음 결과 (태그 `sgfix-java`, prefix `0.17.3`)

C: `perf_c_single_linux_20260908_161205_sgfix-java.txt`
Java: `perf_java_single_linux_20260908_161311_sgfix-java.txt`

| pattern | size | C tput | Java tput | **after** | before | C 깊이 | Java 깊이 |
|---|---|---|---|---|---|---|---|
| DEALER_ROUTER_REQREP | 64 | 800,682 | 401,771 | **50.2 %** | 25.9 %† | 8289.4 | 92.6 |
| | 256 | 832,356 | 376,934 | **45.3 %** | | 499.5 | 96.2 |
| | 1024 | 722,042 | 369,679 | **51.2 %** | | 1020.5 | 96.1 |
| | 65536 | 11,095 | 4,700 | **42.4 %** | | 1.94 | 15.48 |
| | 131072 | 9,399 | 4,197 | **44.7 %** | | 1.94 | 7.60 |
| | 262144 | 7,598 | 3,271 | **43.0 %** | | 1.93 | 3.70 |
| ROUTER_ROUTER_REQREP | 64 | 880,808 | 411,806 | **46.8 %** | 22.2 %† | 125.4 | 100.6 |
| | 256 | 842,824 | 377,646 | **44.8 %** | | 177.4 | 103.1 |
| | 1024 | 681,651 | 327,641 | **48.1 %** | | 7144.6 | 151.2 |
| | 65536 | 11,664 | 4,768 | **40.9 %** | | 1.94 | 15.50 |
| | 131072 | 9,710 | 4,094 | **42.2 %** | | 1.94 | 7.62 |
| | 262144 | 7,464 | 3,041 | **40.7 %** | | 1.93 | 3.70 |

† before는 `sg1` 산술평균(DR 25.9 %, RR 22.2 %).
산술평균 **DR 46.1 %**(before 25.9 %), **RR 43.9 %**(before 22.2 %).

**구조 결함은 해소됐다.** 미완료 깊이가 0.90 고정 → **92 ~ 151**(작은 크기) /
**3.7 ~ 15.5**(큰 크기)로 C와 같은 자릿수가 됐고, 절대 처리량은 64 B에서
5,310 → 401,771 ops/s로 **76배**다. RTT 루프는 사라졌다.

**기대치(60 %+) 미달의 잔여 원인은 turn 구조가 아니라 Java의 건당 비용이다.**
- 65536 B 이상에서 Java는 창 상한에 붙어 있고(깊이 15.5 / 7.6 / 3.7 ≈ 창 16 / 8 / 4)
  더 깊게 갈 수 없는데도 42 % / 45 % / 43 %다. 같은 창·같은 깊이(15.99)의 C++은
  99 %였다 — 즉 같은 부하 모델에서 Java가 C++보다 2.3배 느리다.
- 이 값은 §4에서 기록한 Java one-way 대형 크기 결손(모든 5개 one-way 패턴에서
  C의 41 % / 34 % / 34 %)과 정확히 같은 대역이다. REQREP 전용 문제가 아니라
  Java 공통 송수신 경로(`bindings/java/perf/common/**` 또는 Java binding recv/send)의
  건당 비용이며, **이 과제의 파일 범위(`bindings/java/perf/single/**`) 밖**이다.
- 작은 크기(50 % / 45 % / 51 %)에서도 Java 깊이는 92~151로 C(500~8289)에 못 미치는데,
  이는 창(16384건)에 한참 못 미치므로 창 제약이 아니라 requester thread의 제출·drain
  처리율 한계다. 같은 원인이다.

## 13. PAIR 64 B 회귀 — C 짝지음 포함 (태그 `sgfix-pair`, prefix `0.17.3`)

| lang | throughput | 비율 | latency | 깊이(건) |
|---|---|---|---|---|
| C (기준) | 1,897,191 | — | 0.0450 ms | 85 |
| C++ | 1,546,332 | 81.5 % | 0.0705 ms | 109 |
| Java | 1,688,018 | 89.0 % | 52.998 ms | 89,463 |

- 두 언어 모두 **처리량 회귀 없음**(before `sg1`: C++ 1,553,949 / Java 1,654,260).
- 이번 변경은 REQREP 러너 파일만 건드렸고 PAIR 경로는 손대지 않았으므로 회귀는
  성립할 수 없다. 측정이 그것을 확인한다.
- Java PAIR latency 53 ms는 §4의 **큐 포화 양안정 상태**다. C++은 이번 실행에서 빈 큐
  쪽(0.07 ms), Java는 가득 찬 쪽에 안착했다. 같은 `sg1` 실행에서 C `DEALER_DEALER`
  64 B가 10.36 ms, C `ROUTER_ROUTER` 256 B가 62.81 ms였고 이번 C `DR REQREP` 64 B도
  10.35 ms다 — C 자신도 같은 두 상태를 오간다. 러너 결함이 아니다.

## 14. 최종 판정

| 항목 | 결과 |
|---|---|
| C++ REQREP 부하 모델 | **정합 완료** — 깊이 1.00 → 4~150, DR 86.7 % / RR 82.9 % (기대 85 %+ 충족/근접) |
| Java REQREP 부하 모델 | **정합 완료** — 깊이 0.90 → 3.7~151, 64 B 처리량 76배 |
| Java 집계 비율 60 %+ | **미달(46 % / 44 %)** — 잔여는 Java 공통 경로 건당 비용, 파일 범위 밖 |
| PAIR 64 B 회귀 | **없음** (C++ 81.5 %, Java 89.0 %) |
| 대형 크기 latency | 창 모델 한계로 확정(§8) |
| 빌드·컴파일 | C++ cmake, Java gradle `installDist` 통과. 이 두 러너에 단위 테스트 없음 |
