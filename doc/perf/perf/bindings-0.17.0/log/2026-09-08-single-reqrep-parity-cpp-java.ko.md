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
