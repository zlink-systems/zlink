# C++ routed async send 자체 개선 pass (`DEALER_DEALER`/`tcp`) — 2026-08-23

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
> §5(고정 원칙), §7.4(pass 순서), §9.1.1(Single suite 표)
> 선행 로그: `log/2026-08-23-cpp-dealer-dealer-ceiling.md`(D1 두-홉 결함 제거),
> `log/2026-08-23-cpp-dealer-dealer-tcp-official.md` §재측정(공식 `미달(53.59%)`)
> 브랜치 / HEAD: `codex/bindings-0.12.0-performance` / `dbad74ddd2`
> commit·push 하지 않았다.

## 1. 범위

D1(`log/2026-08-23-cpp-dealer-dealer-ceiling.md`)로 "메시지당 스레드 왕복 2회"라는
구조적 상한은 제거됐지만 공식 재측정은 여전히 `미달(aggregate 53.59%)`이었다.
이번 pass는 계획서 §7.4의 **pattern 전용 자체 개선 pass**로, routed 비동기 send
경로의 **메시지당 고정 비용**을 줄이는 것을 목표로 했다. public API·contract는
바꾸지 않았다(routed send의 유일한 terminal이 `async()`라는 계약 포함).

## 2. 비용 분해 (측정)

`perf`/`valgrind`를 쓸 수 없어 **일회용 rdtsc 계측**(`hot_path_profile.hpp`,
측정 후 삭제)을 `routed_send_submit_operation_t::async()` 경로에 삽입하고
`DEALER_DEALER`/`tcp` 64B(local core, duration 3)를 돌려 구간별 ns/msg를 얻었다.
계측 자체가 메시지당 약 0.3 µs를 더하므로(552,532 → 471,073 msg/s) **절대값이
아니라 구성비**로 읽는다.

개선 전(HEAD `dbad74ddd2`), 64B, 샘플 1,413,252건:

| 구간 | ns/msg | 설명 |
|------|-------:|------|
| **`async()` 전체** | **1882.2** | 하네스 1.81 µs/msg 중 바인딩이 차지하는 부분 |
| ├ `state_alloc` | 22.2 | `make_shared<async_operation_state_t<void>>` |
| ├ `prelude` | 37.4 | callback state share + handler/admission 확보 |
| ├ `select_target` | 133.7 | `outbound_record_attempt_mutex` + `zlink_select_routed_submit_target` |
| ├ `sndtimeo_opt` | 71.0 | 메시지마다 `sndtimeo` 소켓 옵션 조회 |
| ├ `attempt_obj` | 41.8 | `make_shared<managed_send_attempt_t>` + parts 벡터 |
| ├ `enqueue` | **1354.4** | 아래 4개 + 호출부 `std::function` 3개 생성(≈165) |
| │ ├ `enq_alloc` | 73.0 | `make_shared<pending_operation_t>`(당시 크기 ≈300B) |
| │ ├ `enq_register` | 122.2 | `_operations`/`_pending_by_target`/`_ready_set`/`_wake_versions` 갱신 |
| │ ├ `pump` | 958.8 | 아래 5개 + `attempts`/`accepted_actions` 벡터(≈92) |
| │ │ ├ `pump_lock1` | 151.1 | ready target 스캔 lock 구간 |
| │ │ ├ `pump_attempt` | **501.8** | **Core submit 본체**(DONTWAIT) |
| │ │ ├ `pump_lock2` | 127.0 | attempt 후처리 lock 구간 |
| │ │ ├ `pump_tail` | 30.9 | 재스케줄 lock 구간 |
| │ │ └ `pump_complete` | 56.1 | `completion->complete()` |
| │ └ `ticket` | 35.0 | `shared_from_this()` + ticket |
| └ `set_cancel` | 44.9 | `std::function<bool()>`(capture 24B → 힙) |

읽어낸 결론:

- **Core submit은 약 500 ns(전체의 27%)**, 나머지 약 1,380 ns가 바인딩 자체 비용이다.
- 메시지 1건이 힙 할당을 **8~12회** 한다: `async_operation_state_t`,
  `managed_send_attempt_t`, parts 벡터, `std::function` 3개(accepted/terminal/cancel),
  `pending_operation_t`, `_operations` 노드, `_pending_by_target` 노드 + `std::deque`
  (큐가 비면 엔트리를 지우므로 **메시지마다 deque 512B 노드가 새로 생겼다 사라진다**),
  `_ready_set` 노드, `_wake_versions` 노드.
- admission 부기(할당 + lock 구간 + 콜백 생성 + ticket) 합계가 **약 840 ns/msg**로
  최대 항목이다.

## 3. 채택한 변경 (D2~D5, 한 묶음)

네 변경은 서로 맞물려 있어 함께 넣었다. 모두 `zlink::detail` 내부이며 공개
헤더·perf 하네스는 손대지 않았다.

### D2 — 호출 스레드 물리 admission fast path (`pending record`를 만들기 전에 시도)

`routed_admission_state_t::enqueue()`는 이제 대상 target에 **앞선 작업이 없으면**
pending record를 만들기 전에 예약만 잡고 `_mutex`를 놓은 뒤 호출 스레드에서
`request->attempt()`를 실행한다. Core가 즉시 수락하면 record·큐 슬롯·deadline
엔트리·ticket을 **하나도 만들지 않고** terminal에 도달한다. 백프레셔일 때만
`park_locked()`가 pending record를 만들어 그 target 큐의 **맨 앞**에 넣는다.

D1이 넣었던 "ready면 caller가 `pump()`를 대신 돌린다"는 인라인 pump는 이 경로로
대체돼 제거했다.

계약 보존:

- **FIFO**: 예약(`inline_attempts`)이 `_mutex` 아래에서 target에 걸리므로 같은
  target에 동시 진입한 다른 스레드는 예약을 보고 뒤에 줄을 선다. 시도 중에 들어온
  작업은 큐 뒤에, 시도 실패로 park되는 작업은 큐 앞에 놓이므로 순서가 유지된다.
- **caller 점유 없음**: `attempt()`는 `ZLINK_SEND_FLAGS_DONTWAIT` submit이라
  블로킹하지 않는다. 백프레셔면 reactor가 하던 것과 같은 `waiting` 전이를 한다.
- **wake 유실 없음**(§5 참조): 예약이 걸린 target은 큐가 비어 있어도
  `wake_target_locked()`가 `wake_version`을 올린다. park 시점에
  `wake_version > observed_wake`면 `waiting`이 아니라 `ready`로 넣는다.
- **terminal 이벤트 경합**: target별 `terminal_epoch`를 두어, 시도 중에 도착한
  routed terminal 이벤트를 시도 종료 후 감지해 그 errno로 terminal 처리한다
  (기존 코드에서는 등록된 record에만 `forced_terminal`이 찍혔다).
- **shutdown**: 예약은 `_active_pumps`를 함께 올리므로 `shutdown()`의
  `_quiesced` 대기가 인라인 시도를 그대로 기다린다.

### D3 — target별 레코드 1개로 통합 (`routed_target_state_t`)

`_pending_by_target` / `_wake_versions` / `_ready_set`(+`_ready_targets`의 key 복사)
네 컨테이너가 같은 key로 병렬 관리되던 것을 `std::map<key, routed_target_state_t>`
하나로 합쳤다(`queue` / `wake_version` / `terminal_epoch` / `inline_attempts` /
`ready_marked`). `_ready_targets`는 key 복사 대신 map iterator를 담는다.
레코드는 작업이 비어도 **지우지 않고 재사용**하며, 서로 다른 transport pair가
`k_retained_target_records`(32)를 넘을 때만 idle 레코드를 정리한다 —
메시지마다 map 노드와 deque 노드가 생겼다 사라지던 churn이 사라진다.
정보 은닉·중복 제거 측면의 POSDDD 이득이 성능과 별개로 있다.

### D4 — 콜백 3종 `std::function` → `routed_admission_request_t` 인터페이스

`enqueue_routed_admission(owner, target, attempt_fn, accepted_fn, terminal_fn,
deadline, attempt_before_expiry)`를
`enqueue_routed_admission(owner, target, shared_ptr<routed_admission_request_t>)`로
바꿨다. 호출자는 이미 send 1건당 힙 레코드(`managed_send_attempt_t`,
`managed_request_attempt_t`)를 갖고 있으므로 그 레코드가 곧 request가 된다 —
메시지마다 만들던 `std::function` 2개(힙)와 `pending_operation_t`의 3개
`std::function` 슬롯이 shared_ptr 1개로 줄었다.

### D5 — 대기할 때만 timeout 조회 + terminal이면 ticket 없음

`sndtimeo`는 "백프레셔로 기다릴 때 얼마나 기다릴지"만 정하므로
`routed_admission_request_t::deadline()`으로 옮겨 **실제로 대기해야 할 때만**
조회한다(deadline 기준 시각은 여전히 operation 시작 시각). 즉시 수락되는
정상 경로는 소켓 옵션 조회 비용을 내지 않는다. 또한 인라인으로 terminal에
도달한 send는 취소할 pending record가 없으므로 빈 ticket을 돌려주고
`set_cancel()`(capture 24B → 힙 할당) 자체를 건너뛴다.

### 개선 후 재계측 (64B, 같은 방식)

| 구간 | ns/msg |
|------|-------:|
| **`async()` 전체** | **1079.2** |
| ├ `prelude` | 54.9 |
| ├ `select_target` | 137.8 |
| ├ `attempt_obj` | 50.9 |
| └ `enqueue` | 683.6 |
| ..├ 예약 lock 구간 | 86.1 |
| ..├ **Core submit** | **452.3** |
| ..├ 해소 lock 구간 | 31.6 |
| ..└ `completion->complete()` | 44.3 |

`sndtimeo_opt`(71) · `enq_alloc`(73) · `enq_register`(122) · `pump_lock1`(151) ·
`pump_tail`(31) · `ticket`(35) · `set_cancel`(45) 항목이 통째로 사라지고,
Core submit이 이제 `async()`의 42%를 차지한다.

## 4. no-go: 단일 파트 직접 submit

`managed_send_attempt_t::attempt()`가 쓰는
`submit_borrowed_message_array()`는 파트마다 `zlink_msg_init` + `zlink_msg_copy`
+ `zlink_msg_close`를 한다. 동기 terminal(`submit_raw_send_state()`)이 단일
파트에서 그러듯 caller의 native handle을 그대로 넘기고 성공 시 `mark_sent()`
하도록 바꿔 봤다.

- 성능: 64B 955,058 → 988,188 msg/s (+3.5%)
- **contract 위반**: `test_cpp_contract_exact_target_retry` 실패
  (`exact-target retry violation: delivered_to_A=0 rerouted_to_B=0`)

즉 `zlink_dealer_send_transport_pair_part`는 백프레셔 시 caller 메시지를
재시도 가능한 상태로 남겨 주지 않는다 — borrowed 어댑터의 복사는 exact-target
재시도 계약을 지탱하는 필수 요소다. **되돌렸다**(+3.5%는 계약과 바꿀 수 없다).

## 5. 백프레셔/wake 가설 검증 (byte-HWM 의심)

"메시지당 부기가 아니라 byte-HWM 백프레셔·wake 왕복이 gap을 지배한다"는 가설을
두 가지 방법으로 직접 검증했다.

### 5.1 HWM A/B (양쪽 언어 동일 조건)

`PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1`로 `--send-hwm 67108864
--recv-hwm 67108864`(64 MiB, auto-HWM 1 MiB의 64배)를 C와 C++ 모두에 적용
(local core, duration 5, runs 1):

| 대상 | 64B auto → bigHWM | 256B auto → bigHWM | 1024B auto → bigHWM |
|------|------------------:|-------------------:|--------------------:|
| C | 2,679,175 → **3,467,441 (+29.4%)** | 2,007,627 → 1,930,560 (−3.8%) | 1,151,270 → 1,144,317 (−0.6%) |
| C++ (개선 후) | 903,600 → 895,364 (−0.9%) | 806,235 → 809,448 (+0.4%) | 781,352 → 766,088 (−2.0%) |
| C++ (개선 전 = HEAD) | 555,071 → 550,754 (−0.8%) | 536,355 → 530,842 (−1.0%) | 502,665 → 490,506 (−2.4%) |

**C는 64B에서 HWM 압력을 풀면 +29% 빨라지지만, C++는 개선 전·후 모두 완전히
평탄하다.** C++는 HWM에 걸려 느린 것이 아니라 **생산자(바인딩 송신 경로) 자체가
느려서** HWM에 도달하지 못한다.

### 5.2 park/wake 계수 (일회용 계수기)

admission 경로에 일회용 카운터·히스토그램을 넣고 개선 후 코드로 5초씩 측정:

| size | inline_accepted | parked | wake_release | deadline_expiry | park→exit 분포 |
|-----:|----------------:|-------:|-------------:|----------------:|----------------|
| 64B | 4,782,515 | **0** | 0 | 0 | (없음) |
| 256B | 4,054,797 | **1** | 1 | 0 | `<1s` 1건(연결 초기 1회) |
| 1024B | 3,982,163 | **19** | 19 | 0 | 전부 `<1ms` |

**백프레셔 경로는 사실상 한 번도 타지 않는다.** 400만 건 중 park 0~19건,
모두 edge wake로 1 ms 안에 풀렸고 deadline 만료(=wake 유실 신호)는 0건이다.
이중 분포(빠른 mode + 타이머 granularity에 몰린 straggler mode)도 없다.

### 5.3 lost-wake(check-then-park) 코드 검증

의심된 순서(“attempt 거절 → reactor에서 edge 발생 → 그 다음에 `waiting` 전이”)는
**버전 카운터로 닫혀 있다**. 두 경로 모두 같은 뮤텍스 아래에서 관찰-비교한다.

- 인라인 경로: `routed_admission_state.cpp` `enqueue()`가 `_mutex` 아래에서
  `inline_attempts`를 올리고 `observed_wake = wake_version`을 읽는다 →
  잠금 해제 후 attempt → `admit_on_caller_thread()`가 다시 `_mutex`를 잡고
  `park_locked()`에서 `wake_version > observed_wake`면 `waiting`이 아니라
  `ready`로 넣는다(즉시 재시도).
- reactor 경로: `pump()`가 `_mutex` 아래에서 `observed_wake`를 찍고 attempt 후
  같은 잠금 안에서 `wake_version > observed_wake`를 다시 본다.
- 그 사이에 도착한 edge는 `on_event()` → `wake_target_locked()`가 `_mutex`
  아래에서 `++wake_version` 한다. **큐가 비어 있어도 `inline_attempts != 0`이면
  올린다** — 이번 pass에서 인라인 예약을 waker에게 보이게 만든 부분이다.
  (개선 전 코드에서는 attempt 전에 record가 이미 큐에 등록돼 있어 같은 보호가
  성립했다.)
- 만약 유실이 있었다면 `sndtimeo=200ms` deadline으로 ETIMEDOUT이 찍히고
  하네스가 1 ms sleep 후 재시도하므로 §5.2의 `deadline_expiry` 카운터에
  나타난다 — 0건이다.

### 5.4 판정

**bookkeeping-dominated (모든 측정 크기에서). backpressure-wake는 사실상
관여하지 않으며 lost wake도 없다.** 근거: (a) 부기만 줄인 이번 변경으로 64B
+64%, 1024B +63%, (b) HWM 64배 완화에 C++는 개선 전·후 모두 무반응(±2%),
(c) 400만 건 중 park 0~19건·deadline 만료 0건. 공식 재측정에서 256B latency가
25 ms인 것은 wake 지연이 아니라 **느린 생산자가 채워 둔 파이프의 큐잉
지연**(latency ≈ 큐 깊이 / 처리량)이다. 65536B 이상에서는 부기 비중이 작아
개선폭도 작다(+1.2%) — 그 구간은 Core/전송 비용 지배다.

## 6. 탐색 측정 (local core, duration 5, runs 3 median)

| 항목 | before(HEAD `dbad74ddd2`) | after | 변화 |
|------|--------------------------:|------:|-----:|
| `DEALER_DEALER`/`tcp` 64B | 546,414.8 | **898,193.0** | **+64.4%** |
| `DEALER_DEALER`/`tcp` 1024B | 485,623.8 | **791,163.0** | **+62.9%** |
| `DEALER_DEALER`/`tcp` 65536B | 33,028.4 | 33,411.6 | +1.2% |
| `DEALER_ROUTER`/`tcp` 64B | 551,135.8 | **922,150.2** | **+67.3%** |
| `PAIR`/`tcp` 64B(회귀 확인) | 2,092,819.2 | 2,095,282.6 | +0.1%(회귀 없음) |

`PAIR`는 동기 `submit()` 경로라 admission을 쓰지 않는다 — 무변화가 정상이다.

## 7. contract / sample 검증

`bash bindings/cpp/tests/run_tests.sh -DZLINK_CPP_BUILD_SAMPLES=ON`

| 항목 | 결과 |
|------|------|
| contract (`ctest -L contract`) | **12/14** — 실패 `test_cpp_contract_socket`(`submit_error_t ... errno=113`), `test_cpp_contract_request_reply`(`:714 test_routed_send_async_isolates_a_backpressure_from_b`) |
| sample smoke (`ctest -L sample-smoke`) | **6/7** — 실패 `sample_smoke_sample_cpp_dealer_router_recv_sample` |

계획서가 기대하는 pre-existing 패턴(12/14, 6/7)과 실패 지점까지 동일하며 이번
변경으로 늘어난 실패는 없다. routed/dealer/router 전용 contract는 명시적으로
통과했다: `test_cpp_contract_exact_target_retry`,
`test_cpp_contract_exact_request_target`, `test_cpp_contract_flow_state`,
`test_cpp_contract_behavior`, `test_cpp_contract_optimization_guard`.

## 8. 공식 재측정 (release core 0.12.0)

session tag `bindings-0.12.0-official-dd3-20260823`. C를 먼저 완주시키고 이어서
C++를 같은 조건으로 실행했다(§7.3). 다른 perf 프로세스 동시 실행 없음
(시작 시 load average 0.94, memory 10Gi free).

| 항목 | 값 |
|------|-----|
| host / OS | `ulalax-gram`, `Linux 6.6.87.2-microsoft-standard-WSL2`(WSL2), 16 logical cores(i7-1260P) |
| 브랜치 / HEAD | `codex/bindings-0.12.0-performance` / `dbad74ddd2` + 본 pass 미커밋 변경 |
| Core runtime | release `--core-version 0.12.0`, `/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64/lib/libzlink.so.0.12.0` |
| Core provenance | `META,core_revision,f99703c2190b0f6c670be49f67315d904886c742`, `core_dirty,0`, `core_release_tag,core/v0.12.0` |
| smoke(§7.1) | C 2,541,295.0 / C++ 883,777.0 msg/s → 34.8%, 둘 다 `status: complete` |
| 전체 크기 report | C `perf_c_single_linux_20260823_164526_bindings-0.12.0-official-dd3-20260823.txt`(complete, 30/30), C++ `perf_cpp_single_linux_20260823_164704_bindings-0.12.0-official-dd3-20260823.txt`(complete, 30/30) |

Effective Options는 `lang`을 빼면 두 report가 일치한다. 단, C++ report에는
`patterns:`와 값이 같은 `pattern: DEALER_DEALER` 한 줄이 더 있다(C++ 러너의 기존
출력 차이이며 조건 차이가 아니다).

| Size | C median | C++ median | 비율 | 직전 공식(dd2) | C latency | C++ latency | latency 비율 |
|-----:|---------:|-----------:|-----:|---------------:|----------:|------------:|-------------:|
| 64B | 2,727,949.0 | 867,119.8 | **31.79%** | 19.84% | 50.428 ms | 43.735 ms | 0.867배 |
| 256B | 2,033,710.6 | 777,464.2 | **38.23%** | 25.80% | 18.759 ms | 25.555 ms | 1.362배 |
| 1024B | 1,154,554.4 | 769,601.8 | **66.66%** | 41.11% | 8.553 ms | 11.557 ms | 1.351배 |
| 65536B | 46,226.6 | 35,120.8 | **75.98%** | 71.76% | 3.336 ms | 4.436 ms | 1.330배 |
| 131072B | 27,077.4 | 22,597.8 | **83.46%** | 80.11% | 2.869 ms | 3.420 ms | 1.192배 |
| 262144B | 16,154.6 | 13,577.4 | **84.05%** | 82.92% | 2.427 ms | 2.806 ms | 1.156배 |

- throughput aggregate mean = **63.36%** (직전 53.59% → **+9.77%p**)
- latency aggregate mean = **1.210배** (상한 2.0배 이내, 개별 최댓값 1.362배)

## 9. 판정

`DEALER_DEALER`는 §2.1 "단순 one-way" 그룹(최소 85% / aggregate 95%, 완화 90%).

- **Throughput 미달**: aggregate mean 63.36%는 목표에 못 미치고, 개별 최소
  기준 85%도 6개 크기 전부 미달이다(최댓값 262144B 84.05%).
- **Latency 통과**: aggregate 1.210배로 상한 2.0배 이내.
- 65536B 국소 dip 없음(31.79 → 38.23 → 66.66 → 75.98 → 83.46 → 84.05%로 단조
  증가) → `/usr/bin/time -v` 환경-지배 재확인 절차 비적용.

**종합: 미달(63.36%)**. 자체 개선 pass는 이번으로 완료됐고 Sol 리뷰 pass가
아직 남아 있으므로 §8 규칙상 `보류`가 아니라 `미달`로 기록한다.

## 10. 남은 gap의 성격 (Sol 리뷰 입력)

개선 후 64B `async()` 1,079 ns 중 **Core submit이 452 ns**다. 남은 바인딩 비용
약 600 ns의 큰 조각은:

1. `select_routed_submit_target` 약 138 ns — 메시지마다
   `outbound_record_attempt_mutex`를 잡고 Core에 target을 물어본다.
2. admission 예약/해소 두 번의 lock 구간 약 118 ns (+ target key의 `std::string`
   구성과 map 조회).
3. 메시지당 남은 힙 할당 3건: `async_operation_state_t`,
   `managed_send_attempt_t`, parts `std::vector`.

구조적으로 더 큰 항목은 **C 하네스와 Core 호출 자체가 다르다**는 점이다. C는
load-balancing `zlink_send_part` 계열을 쓰는 반면 C++ DEALER 비동기 send는
계약상 "target 선택 → exact transport-pair submit"을 메시지마다 수행한다
(`select` 138 ns + 더 무거운 submit). 이 차이를 없애려면 admission의 target
keying 모델 자체를 바꿔야 하므로 이번 pass 범위 밖으로 남긴다 — Sol 리뷰에서
계약 보존 가능성을 판단할 후보다.

## 11. 코드·commit 상태

수정 파일(모두 `zlink::detail` 내부, 공개 헤더·perf 하네스 무변경):

- `bindings/cpp/src/Runtime/Messaging/routed_admission_state.hpp`
  — `routed_admission_request_t` 인터페이스, `routed_deadline_t`,
    `routed_admission_ticket_t::valid()`, `enqueue_routed_admission()` 시그니처
- `bindings/cpp/src/Runtime/Messaging/routed_admission_state.cpp`
  — 인라인 admission fast path, target 레코드 통합, request 인터페이스 적용
- `bindings/cpp/src/Runtime/Messaging/send_operations.cpp`
  — `managed_send_attempt_t`가 request를 구현, lazy `deadline()`, ticket 유효성 검사
- `bindings/cpp/src/Runtime/Messaging/request_reply.cpp`
  — `managed_request_attempt_t`가 request를 구현(동일 fast path 수혜)

일회용 계측(`hot_path_profile.hpp`, admission 카운터)은 측정 후 모두 제거했다.
commit·push 하지 않았다.
