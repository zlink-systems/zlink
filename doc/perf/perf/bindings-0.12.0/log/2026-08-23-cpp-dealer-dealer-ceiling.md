# C++ `DEALER_DEALER`/`tcp` 약 10K msg/s 고정 상한의 근본 원인과 수정 (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md` §5(수정 범위), §7.4(pass 순서)
> 선행 로그: `log/2026-08-23-cpp-dealer-dealer-tcp-official.md`(공식 paired 측정, 미달 19.02%)
> 브랜치 / HEAD: `codex/bindings-0.12.0-performance` / `8a5a0361da`
> commit·push 하지 않았다.

## 1. 증상

공식 paired 측정(`log/2026-08-23-cpp-dealer-dealer-tcp-official.md` §3~§4)에서
C++ `DEALER_DEALER`/`tcp` throughput이 메시지 크기와 **무관하게** 약
9,000~10,000 msg/s로 평탄했다.

| Size | C median | C++ median | 비율 |
|-----:|---------:|-----------:|-----:|
| 64B | 2,633,372.4 | 9,849.4 | 0.37% |
| 256B | 2,003,676.2 | 9,822.0 | 0.49% |
| 1024B | 1,149,967.0 | 9,818.2 | 0.85% |
| 65536B | 46,352.0 | 10,018.2 | 21.61% |
| 131072B | 27,449.4 | 9,620.6 | 35.05% |
| 262144B | 16,205.4 | 9,037.2 | 55.77% |

- C는 크기가 커질수록 자연스럽게 감소하지만 C++는 전 크기에서 평탄하다 —
  대역폭 한계가 아니라 **초당 요청 수 자체의 상한**이다.
- 64B에서 9,849 msg/s ≈ **메시지당 약 102 µs**, 관측 latency mean도
  0.115 ms로 거의 같다. 즉 in-flight 메시지가 사실상 1건뿐인
  **메시지 단위 직렬화(lockstep) 서명**이다.
- 이 상한은 이번 세션 변경 이전부터 존재했다(`log/2026-08-23-cpp-c1-c3-c4-c5-implementation.md`
  §4.4: C1~C5 전 9,791.6 / 후 9,735.2 msg/s로 동일).

## 2. 조사 경로

### 2.1 하네스 비교 — 측정 의미는 동일하다

| 항목 | C (`bindings/c/perf/single/src/perf_dealer_dealer.cpp`) | C++ (`bindings/cpp/perf/single/src/perf_dealer_dealer.cpp`) |
|------|------|------|
| 토폴로지 | DEALER bind(receiver) ← DEALER connect(sender), one-way | 동일 |
| 송신 루프 | `send_socket_active_message(..., ZLINK_SEND_FLAGS_NONE, retry_on_eagain=true)` 후 즉시 다음 메시지 | 메시지마다 stamp → 전송 → 즉시 다음 메시지 (`:79-98`) |
| 수신 루프 | poller 대기 → `DONTWAIT` 드레인 | 동일 (`:134-180`) |
| 종료 | 단일 stop token | 동일 (`:101`) |

C++ 하네스에는 **per-message echo/ack, 응답 대기, sleep, 100 µs poll timeout,
credit 대기가 없다**. `send_rc == 0`(backpressure) 경로에만 1 ms sleep이
있으나 이는 C의 `retry_on_eagain`과 대응하며 실제로는 거의 타지 않는다
(타면 1,000 msg/s가 되지 목표 상한 9,800이 나오지 않는다). 즉 **하네스의
측정 의미는 C와 동일**하고, 계획서 §5의 "perf 수정 허용" 사유(측정 의미 차이)에
해당하지 않는다.

유일한 구조적 차이는 **송신 API**다. C++ `dealer_socket_t::send()`는
`routed_send_operation_t`를 돌려주고, 그 유일한 terminal은 `async()`다
(`bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp:244`
"Builds a DEALER/ROUTER send whose only terminal is async()"). 하네스는
공개 계약상 다른 선택지가 없어 `perf_socket_adapter.hpp:622 send_async()` →
`co_await`를 쓴다. 즉 **DEALER/ROUTER 송신은 반드시 비동기 admission 경로를
통과**한다(PAIR/PUB는 동기 `submit()`을 쓰며 정상 성능이 나온다).

### 2.2 근본 원인 — 메시지당 2회의 스레드 왕복

메시지 1건의 `co_await send_async(msg)` 경로:

1. `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:406`
   `routed_send_submit_operation_t::async()` → `start_managed_send()`(`:110`).
2. `start_managed_send` → `detail::enqueue_routed_admission()` →
   **`bindings/cpp/src/Runtime/Messaging/routed_admission_state.cpp:145`
   `routed_admission_state_t::enqueue()`**.
3. `enqueue()`는 pending record를 등록하고 `refresh_schedule_locked()`
   (`:517`)를 호출한다. ready 상태이므로 `due = now`로
   `admission_reactor().schedule(...)`(`:531`)을 부르고 **즉시 반환한다.
   호출 스레드는 실제 전송을 한 번도 시도하지 않는다.**
4. 전용 reactor 스레드(`routed_admission_reactor_t::run()`, `:638`)가
   condition variable에서 깨어나 `pump()`(`:292`)를 돌리고, 거기서
   비로소 `managed_send_attempt_t::attempt()`(`:29`)가
   `zlink_dealer_send_transport_pair_part(..., ZLINK_SEND_FLAGS_DONTWAIT, ...)`를
   호출한다. → **1차 스레드 왕복**
5. 성공하면 accepted 콜백이 `completion->complete()`를 부르고,
   `async_operation_state.hpp:176 resume()` → `dispatch_async_continuation()` →
   `async_continuation_dispatcher.cpp`의 별도 워커 스레드 큐로 post된다.
   그 워커가 깨어나 coroutine을 resume 한다. → **2차 스레드 왕복**
6. resume 된 sender coroutine이 다음 메시지를 만든다.

하네스는 매 메시지를 `co_await` 하므로 이 6단계가 **완전히 직렬화**된다.
메시지 1건 = CV notify 2회 + 컨텍스트 스위치 2회 + `shared_ptr`/`std::function`
힙 할당 다수. WSL2에서 이 왕복 비용이 메시지당 약 100 µs이고, 이것이 곧
크기와 무관한 약 10K msg/s 상한이다. 메시지 크기가 커질수록 비율이 개선되는
것(0.37% → 55.77%)은 고정 상한 위에 실제 전송 비용이 더해지면서 C도 함께
느려지기 때문이다.

`DEALER_ROUTER`도 같은 `routed_send_operation_t::async()` 경로를 쓰므로
동일 결함을 공유한다(§4.3에서 실측 확인).

**결론: 하네스 결함이 아니라 바인딩 내부 결함이다** — 계약상 DEALER 송신의
유일한 terminal이 `async()`인데, 그 `async()`가 백프레셔가 전혀 없는
정상 케이스에서도 물리 admission을 항상 reactor 스레드에 위임한다.

## 3. 수정

`bindings/cpp/src/Runtime/Messaging/routed_admission_state.cpp:145`
`routed_admission_state_t::enqueue()`에 **물리 admission fast path**를 추가했다.

새 operation이 해당 target의 **유일한** pending work일 때(`first_for_target`,
즉 `state == ready`), reactor에 schedule 하는 대신 같은 스케줄 상태
(`_scheduled` / `_scheduled_due` / `_schedule_generation`)를 세팅한 뒤
`_mutex`를 놓고 **호출 스레드에서 `pump(generation)`을 직접 실행**한다.
ready가 아니면(앞에 대기 중인 operation이 있으면) 기존대로
`refresh_schedule_locked()`로 reactor에 넘긴다.

계약 보존 근거:

- **FIFO 순서**: fast path는 해당 target 큐에 앞선 operation이 없을 때만
  타므로 target별 순서가 뒤바뀔 수 없다.
- **"caller를 점유하지 않는다"**: `attempt()`는 `ZLINK_SEND_FLAGS_DONTWAIT`
  Core submit이라 블로킹하지 않는다. 백프레셔면 `pump()`가 operation을
  `waiting`으로 두고 deadline을 등록한다 — reactor가 하던 것과 완전히 동일한
  상태 전이이므로 "HWM credit이 없는 동안 caller/worker 스레드를 점유하지
  않는다"는 `operation_contracts.hpp:231-234` 계약이 유지된다.
- **시그니처·타입 무변경**: `async()`는 여전히 `async_result_t<void>`를
  돌려준다. 즉시 수락된 경우 결과가 이미 terminal이므로
  `async_result_t::awaiter_t::await_ready()`(`operation_contracts.hpp:76`)가
  `true`가 되어 coroutine이 아예 suspend 하지 않는다. 그 결과
  continuation dispatcher hop도 사라진다(`_continuation`이 아직 없으므로
  `async_operation_state.hpp:176 resume()`이 즉시 반환).
- **취소/타임아웃**: 인라인 완료 시 `completion->set_cancel()`과
  `async_result_t` 소멸자의 `cancel()`은 `_terminal` 검사로 no-op이 된다.
  deadline 등록·만료 처리는 `pump()`가 그대로 수행한다.
- **락 순서**: `attempt()`는 `pump()`가 `_mutex`를 놓은 뒤 호출되므로
  `_mutex` → `outbound_record_attempt_mutex` 중첩이 생기지 않는다
  (reactor 스레드와 동일한 순서).

수정 파일은 이 한 곳뿐이다(하네스·공개 헤더 무변경).

## 4. 검증

환경: `ulalax-gram`, WSL2 `Linux 6.6.87.2-microsoft-standard-WSL2`,
16 logical cores(i7-1260P). local core(`ZLINK_CORE_SOURCE=local`),
`--duration 5 --runs 1` 탐색 측정.

### 4.1 `DEALER_DEALER`/`tcp` (핵심)

| Size | C++ before | C++ after | 개선 | C 기준(local) | after 비율 |
|-----:|-----------:|----------:|-----:|--------------:|-----------:|
| 64B | 9,732.0 | **544,992.0** | **×56.0** | 2,685,831.8 | 0.36% → **20.29%** |
| 65536B | 10,056.4 | **31,328.8** | **×3.1** | 44,014.6 | 22.8% → **71.18%** |

- before: `perf_cpp_single_linux_20260823_155050_dd-baseline.txt`
- after: `perf_cpp_single_linux_20260823_155213_dd-fix.txt`(1차, 580,713.6 / 33,109.2),
  `..._dd-fix-final.txt`(재빌드 후 재확인, 544,992.0 / 31,328.8)
- C 기준: `perf_c_single_linux_..._dd-cref.txt`

크기 무관 평탄 현상이 사라지고 **C와 같은 방향으로 크기에 따라 감소**한다
(64B 545K → 65536B 31K). latency도 65536B에서 0.127 ms → 4.923 ms로
C(3.493 ms)와 같은 자리수가 됐다 — before의 비정상적으로 낮은 latency가
"in-flight 1건" 부작용이었음이 확인된다.

### 4.2 `PAIR`/`tcp` 64B 부수 회귀 스모크

| | throughput | latency mean |
|---|---:|---:|
| after | 2,130,965.0 ~ 2,206,781.0 msg/s | 62.4~64.7 ms |

PAIR는 동기 `submit()` 경로라 admission state를 쓰지 않는다. 값은 기존
`PAIR` 측정 범위(약 2.06~2.17M msg/s) 안이고 회귀 없음.

### 4.3 `DEALER_ROUTER`/`tcp` 64B — 동일 결함 공유 확인

같은 파일만 `git stash`로 되돌려 before/after를 직접 비교했다.

| | throughput | latency mean |
|---|---:|---:|
| before | 9,748.0 | 0.117 ms |
| after | **575,476.2** (×59.0) | 5.646 ms |
| C 기준 | 2,654,688.0 | 36.720 ms |

before가 `DEALER_DEALER`와 소수점까지 같은 자리(9.7K, 0.117 ms)였다는 점이
동일 원인임을 뒷받침한다.

### 4.4 contract / sample smoke

`bash bindings/cpp/tests/run_tests.sh -DZLINK_CPP_BUILD_SAMPLES=ON`

| 항목 | before(동일 파일 stash) | after |
|------|------|------|
| contract (`ctest -L contract`) | 12/14 — 실패 `test_cpp_contract_socket`, `test_cpp_contract_request_reply` | **12/14 — 동일한 2건** |
| sample smoke (`ctest -L sample-smoke`) | — | **6/7 — 실패 `sample_smoke_sample_cpp_dealer_router_recv_sample`(기존)** |

두 contract 실패는 before 빌드에서도 **같은 지점**에서 재현된다:

- `test_cpp_contract_socket`: `terminate called ... zlink::submit_error_t: No such file or directory (errno=113)`
- `test_cpp_contract_request_reply`: `test_cpp_contract_request_reply.cpp:714`
  `test_routed_send_async_isolates_a_backpressure_from_b()` assertion

즉 계획서가 기대하는 pre-existing `12/14`, `6/7` 패턴 그대로이며 이번 수정으로
늘어난 실패는 없다.

## 5. 남은 gap과 다음 조치

수정 후에도 64B에서 C 대비 약 20%다. 상한 성격의 결함은 제거됐지만,
메시지마다 `pending_operation_t`/`shared_ptr`/`std::function` 3~4회 힙 할당,
`std::map`/`std::deque`/`std::set` 갱신, `async_operation_state_t` 생성이
남아 있어 이는 별도의 자체 개선 pass(§7.4) 주제다. 이번 로그의 범위는
"평탄한 10K 상한"이라는 기능적 결함의 근본 원인 제거까지다.

- `DEALER_DEALER`/`tcp` 공식 paired 측정(`--core-version 0.12.0`, 전체 크기,
  `--runs 3`)은 **재측정이 필요하다**. 기존 `미달(19.02%)` 값은 이 결함이
  있는 상태의 값이다.
- `DEALER_ROUTER`(및 `DEALER_ROUTER_REQREP` / `ROUTER_ROUTER` 계열 등
  `routed_send_operation_t::async()`를 쓰는 모든 pattern)도 이 수정의
  수혜 대상이다. 공식 측정 전 동일 확인이 필요하다.
- `publish_admission_state`(PUB/XPUB의 `async()` 경로)는 같은 구조를 가지나
  현재 `PUBSUB` 하네스가 동기 `publish().submit()`을 쓰므로 이번 측정에는
  영향이 없다. 동일 fast path 적용 여부는 별도 판단 대상으로 남긴다.

## 6. 코드·commit 상태

- 수정 파일: `bindings/cpp/src/Runtime/Messaging/routed_admission_state.cpp`
  (`enqueue()` 한 곳)
- 공개 헤더·perf 하네스는 수정하지 않았다.
- commit·push 하지 않았다.
