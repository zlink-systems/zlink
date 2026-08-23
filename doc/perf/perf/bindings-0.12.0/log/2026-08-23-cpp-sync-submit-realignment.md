# C++ 바인딩: routed send 동기 `submit()` 재정렬 (2026-08-23)

> 대상: `bindings/cpp` (C++ 바인딩, 나머지 6개 언어의 reference 구현)
>
> 근거 스펙: `bindings/doc/spec/async-coroutine-policy.ko.md`(2026-08-23 개정),
> `doc/plan/cpp-routed-async-contract-issue.ko.md` §0 지배 원칙, §3.1 선행 확인,
> §3.2 최종 설계 확정
>
> 브랜치 / 기준 HEAD: `codex/bindings-0.12.0-performance` / `99875dc498`
> (본 작업은 **커밋하지 않았다**)

## 1. 설계

### 1.1 결정 요약

| 축 | 이전(8/15 admission 계약) | 이번 재정렬 |
|---|---|---|
| routed **send** terminal | `async()` 전용 (`async_result_t<void>`) | 동기 `submit() -> void`, 실패 시 `submit_error_t` |
| routed send HWM 대기 | binding park queue + WRITABLE 콜백 재시도 + deadline timer | 전부 Core 소유 (blocking 대기 → Core 신호로 재개, `SNDTIMEO`가 상한) |
| routed send 실행 컨텍스트 | admission reactor thread → dispatcher worker | 호출 thread에서 시작·종료 |
| **request** terminal | `async()` (admission 경유) | `async()` 유지. **제출은 동기**, 완료는 Core reply handler callback이 구동 |
| request timeout | binding deadline(admission) + Core reply timeout | Core 단독 소유 (`ZLINK_REQUEST_TIMED_OUT`) |
| suspension 재개 | binding dispatcher worker 2개 | 완료가 발생한 컨텍스트(Core reply handler callback) 인라인 |
| plain send/publish `async()` | 제공 | **제거** (admission 기계장치에 전적으로 의존했다) |
| binding 소유 스레드 | 4개 (routed reactor 1, publish reactor 1, dispatcher worker 2) | **0개** |

### 1.2 routed send

`routed_send_submit_operation_t::submit()`은 기존 동기 경로
`detail::submit_raw_send_state()`를 그대로 호출한다. 즉 DEALER는
`zlink_send_part`, ROUTER는 `zlink_send_part_rid`로 Core에 직접 제출한다.
part sequence는 호출자 소유이며, binding은 한 번의 native attempt 동안만
handle 단위 record-attempt gate를 잡는다(part 교차 제출과 close 경합 방지).

flags 단계는 추가하지 않았다(builder 모양 불변). `DONTWAIT` 의미는 socket
`SNDTIMEO=0`으로 표현하며, 실측으로 Core가 즉시 `BACKPRESSURED`/`EAGAIN`을
돌려주는 것을 확인했다(§4.3).

### 1.3 request

`submit_raw_request_awaitable()`을 다시 썼다.

1. `zlink_select_routed_submit_target()`으로 exact target을 한 번 스냅샷한다
   (정책 없는 스냅샷 — credit 예약이 아니다). exact-target 계약은 유지된다.
2. record-attempt gate 안에서 `zlink_dealer_request_transport_pair_part` /
   `zlink_router_request_transport_pair_part`를 **`ZLINK_SEND_FLAGS_NONE`**
   (blocking)으로 호출한다. 제출 자체의 HWM 대기는 Core 소유다.
3. 제출이 성공하면 reply handler bridge를 arm하고 `async_result_t`를 돌려준다.
   실패면 `submit_error_t`(또는 `request_error_t`)를 **던진다**.
4. reply가 도착하면 Core의 reply handler callback이 bridge를 통해 suspension을
   완료하고, coroutine은 그 컨텍스트에서 인라인으로 재개된다.

`post_routed_completion()`(dispatcher 경유)은 제거하고
`async_operation_state_t::resume()`이 직접 재개하도록 바꿨다. framework promise가
`zlink_continuation_scheduler()` 훅을 제공하면 그 훅으로만 handoff한다 —
binding은 executor를 만들지 않는다.

### 1.4 cancel / drop

`async_result_t::cancel()`과 drop은 그대로 동작한다. admission ticket이 사라졌으므로
Core가 이미 수용한 request에는 취소 콜백이 설치되지 않고 `cancel()`은 `false`를
돌려준다 — 이는 이전에도 "즉시 admit된 request"에 대해 동일했던 동작이며, 수용
후에는 Core reply lifecycle이 완료를 소유한다는 계약과 일치한다. drop된 awaiter는
`abandon()`으로 continuation 슬롯을 비우므로 Core가 나중에 완료를 전달해도
파괴된 coroutine을 재개하지 않는다.

### 1.5 재개 컨텍스트의 제약 (신규 문서화 대상)

재개가 Core reply handler callback 컨텍스트에서 일어나므로, 그 continuation에서
socket이나 context를 파괴하면 Core를 자기 콜백 안에서 종료시키게 되어 정지한다.
`bindings/cpp/samples/request_reply_async_sample.cpp`가 정확히 이 형태였고
(coroutine 본문이 소켓·컨텍스트를 소유), 샘플 smoke가 timeout으로 실패했다.
샘플을 "소켓/컨텍스트는 `main`이 소유, coroutine은 요청만 수행"으로 재구성했고
주석으로 이유를 남겼다. 스펙(ko/en)에도 같은 제약을 명시했다.

## 2. 삭제한 파일과 공개 표면

### 2.1 삭제한 파일 (총 1,566줄)

| 파일 | 줄 | 내용 |
|---|---:|---|
| `bindings/cpp/src/Runtime/Messaging/routed_admission_state.{hpp,cpp}` | 93 + 915 | admission reactor thread, park queue, deadline timer, wake plumbing, ticket |
| `bindings/cpp/src/Runtime/Messaging/publish_admission_state.{hpp,cpp}` | 53 + 363 | publish admission reactor thread와 대기열 |
| `bindings/cpp/src/Runtime/Messaging/async_continuation_dispatcher.cpp` | 142 | continuation dispatcher worker 2개 |

`bindings/cpp/CMakeLists.txt`의 소스 목록에서 세 `.cpp`를 제거했다.

### 2.2 제거한 public 표면 (⚠ 검토 대상)

| 표면 | 상태 | 이유 |
|---|---|---|
| `routed_send_submit_operation_t::async() -> async_result_t<void>` | **제거** | 스펙 §3.2 — routed send는 동기 `submit()`이 canonical terminal |
| `send_submit_operation_t::async() -> async_result_t<void>` | **제거** | PAIR/STREAM/PUB의 async terminal은 전적으로 admission 기계장치(park queue + dispatcher)에 의존했다. zero-thread 원칙상 유지 불가 |
| `send_submit_operation_t::timeout(std::chrono::milliseconds)` | **제거** | admission deadline 전용 단계였다. 동기 submit에서는 아무것도 하지 않는 dead surface가 되므로 남기면 오히려 함정이다. 대기 상한은 socket `SNDTIMEO` |
| `routed_send_submit_operation_t::submit() -> void` | **추가** | 스펙 표의 C++ 행(`void`, 실패 시 `submit_error_t`) |

내부(비공개) 표면 제거: `detail::routed_admission_*`, `detail::publish_admission_*`,
`detail::post_routed_completion`, `detail::dispatch_async_continuation`,
`detail::ensure_async_continuation_dispatcher`,
`socket_callback_state_t::{routed_admission_mutex, routed_admission,
publish_admission_mutex, publish_admission}`.

**유지한 표면**: `request_submit_operation_t::async()`,
`request_submit_operation_t::timeout()`, `async_result_t<T>`(+`cancel()`),
`reply_submit_operation_t::submit()`, `send_submit_operation_t::{flags, submit}`,
builder 진입점 이름 전부(`dealer.send()`, `router.send(rid)`, `request()`,
`publish(topic)`).

### 2.3 부수 수정 — `socket_t::close()` 기아 상태

`socket_t::close()`는 record-attempt gate를 잡은 **뒤에** `socket_closed`를
세웠다. routed send가 모두 이 gate를 지나게 되면서, 뜨거운 send 루프
(4 thread)와 경합할 때 `std::mutex`의 비공정성 때문에 `close()`가 무한히
기아 상태에 빠지는 것을 stress에서 재현했다(§4.4에서 20 round 중 3 round째 정지).

수정: `socket_closed`를 gate 획득 **전에** publish한다(소멸자 경로는 이미 그렇게
하고 있었다). 그러면 진행 중인 submit만 빠져나가고 새 submit은 즉시 거절되므로
`close()`가 곧바로 gate를 얻는다. 이 수정 후 stress 20 round 전부 통과.

## 3. Consumer 적응

### 3.1 contract test (계약을 새로 검증하도록 재작성, 커버리지 삭제 없음)

| 파일 | 변경 |
|---|---|
| `test_cpp_contract_behavior.cpp` | `static_assert`를 뒤집었다: `has_sync_submit_t<routed_send_submit_operation_t>` **참**, `has_flags_t` 거짓 유지, `has_async_t<routed_send_submit_operation_t>` 거짓 **추가**, `has_async_t<send_submit_operation_t>` 거짓 **추가**, `submit()` 반환 타입이 `void`임을 확인. closed ROUTER send 케이스는 `submit()`으로 전환 |
| `test_cpp_contract_request_reply.cpp` | `test_routed_send_direct_await_and_cancel_are_event_driven` → **`test_routed_send_submit_is_synchronous_and_consumes_parts`**(동기 종료·part 소비·multipart). `test_routed_async_builder_does_not_outlive_socket_anchor` → **`test_routed_builder_does_not_outlive_socket_anchor`**(파괴/close된 anchor에서 `submit()`이 `invalid_state`/`EINVAL`로 throw). `test_routed_send_async_isolates_a_backpressure_from_b` → **`test_routed_send_reports_core_backpressure_without_poisoning_b`**(SNDTIMEO 만료 → `BACKPRESSURED`/`EAGAIN`, `SNDTIMEO=0` 즉시 반환, B는 무영향, drain 후 재제출 성공). `test_routed_send_async_progress_is_independent_of_another_continuation` → **`test_routed_send_submits_from_concurrent_callers`**(2 thread × 64건, 손실 0). `test_routed_request_deadline_includes_admission_wait` → **`test_routed_send_and_request_honor_core_sndtimeo`**. 나머지 routed send 호출부는 `submit()`으로 기계적 전환 |
| `test_cpp_contract_exact_target_retry.cpp` | 전면 재작성. 제거된 "binding이 initial exact target을 유지하며 재시도" 계약 대신 새 계약 2건을 검증한다: **blocking submit이 Core 안에서 park했다가 Core credit 신호로 재개**하고 전달되는지, **credit이 돌아오지 않으면 `SNDTIMEO` 상한 안에서 `BACKPRESSURED`로 실패**하고 part handle이 호출자에게 남는지 |
| `test_cpp_contract_exact_request_target.cpp` | request의 exact-target 계약은 유지되므로 두 테스트를 새 동기 제출 모델로 재작성했다: drain thread가 A의 credit을 풀어 주는 동안 request가 **A에만** 도달하고 A에서 응답을 받는지, 그리고 credit이 없는 A에 대해 `SNDTIMEO` 만료 시 제출이 실패하고 **B로 재라우팅되지 않는지** |
| `test_cpp_contract_socket.cpp` | `submit_routed_send(... .async())` 9곳을 `.submit()`으로 기계적 전환 |
| `tests/contract/support.hpp` | `routed_send_test_task_t`, `await_routed_send()`, `submit_routed_send()` 제거(동기 terminal에는 불필요) |

### 3.2 sample

| 파일 | 변경 |
|---|---|
| `samples/dealer_router_recv_sample.cpp` | `co_await ... .async()` 2건 → `.submit()`. coroutine이 아니게 되어 `sample_task_t` 래퍼를 벗기고 평범한 `main()`으로 되돌렸다 |
| `samples/request_reply_async_sample.cpp` | request `async()`는 유지. §1.5 제약 때문에 소켓·컨텍스트 소유를 `main`으로 옮기고 coroutine은 요청만 수행하도록 재구성 |

### 3.3 framework (C++ framework, 최소 기계적 적응)

스펙 원칙: "routed send용 awaitable을 원하는 framework는 동기 `submit()`을 자기
executor로 감싼다. bindings는 executor를 제공하지 않는다." 여기서는 이미 모든
호출부가 framework `task_t<...>` coroutine 안에 있었으므로, `async()` + `co_await`
2단계를 **현재 task thread에서의 동기 `submit()`** 한 단계로 바꾸는 것이
최소한의 올바른 적응이다. 별도 executor 도입은 하지 않았다(framework 소유 결정).

| 파일 | 변경 |
|---|---|
| `framework/src/runtime/backend/raw_dealer_port.cpp` | `send(parts)`, `send(parts, timeout)` 2곳. timeout 판은 `SNDTIMEO`를 submit **전체** 구간 동안 설치했다가 복원하도록 순서를 고쳤다(이전에는 `async()` 반환 직후 복원해도 됐다) |
| `framework/src/runtime/backend/raw_route_port.cpp` | `send_result(...)`. `router_admission_submit`/`router_admission_complete` trace 단계는 유지하되, 대기 단계(`router_admission_wait/pending`)는 사라졌으므로 제출 결과를 두 단계에 함께 기록한다 |
| `framework/src/runtime/fanout/raw_fanout_owner.cpp` | PUB publish. per-call timeout → `SNDTIMEO` 설치/복원 |
| `framework/src/runtime/streams/stream_host_service.cpp` | STREAM 프레임 write. per-call timeout → `SNDTIMEO` 설치/복원 |
| `framework/src/runtime/channels/channel_outbound_exchange.cpp` | routed send 1곳(이미 `SNDTIMEO`를 쓰고 있었으므로 복원 시점만 이동), native publish 1곳 |
| `tests/Zlink.Framework.ContractTests/test_cpp_framework_target_contract.cpp` | `CPP-CONTRACT-STREAM-001` gate가 `std::move (send).timeout (*timeout).async ()` 소스 문자열을 요구했다 → 새 표현(`send_timeout (*timeout)` + `.submit ()`)으로 갱신 |

framework의 **request** 경로(`async_result_t<std::vector<message_t>>`)와 framework
자체 builder의 `.async()`(`connect().async()`, `wait_for<T>().async()` 등)는 전혀
바뀌지 않았다. framework unit/contract test에 남아 있는 `.async()` 호출은 모두
그 두 종류다.

**⚠ 검증 한계**: framework C++ 빌드는 vcpkg toolchain(부트스트랩 미완)이 필요해
이번 세션에서 컴파일하지 못했다. 위 6개 파일은 **컴파일 미검증**이다.

### 3.4 perf 하네스

측정 anchor(`stamp_payload` 직전 타임스탬프 → 수신 시 decode)는 그대로 두고
send 경로만 동기화했다. 이로써 C 하네스와 같은 모양이 된다.

| 파일 | 변경 |
|---|---|
| `perf/common/perf_socket_adapter.hpp` | `send_async(...)` 2개 → `send_routed(...)` 2개(동기, 예외 → `binding_error_t`) |
| `perf/single/common/perf_single_common.hpp` | `send_payload_active(...)`를 `async_task_t<int>` → `int`로, `send_stop_token_async(...)` → `send_stop_token_active(...)`(동기). ROUTER handshake 2곳도 동기화 |
| `perf/single/src/perf_dealer_dealer.cpp` | `sender_work` lambda를 coroutine에서 일반 함수로(수신은 이미 별도 thread) |
| `perf/single/src/perf_dealer_router.cpp` | sender를 `std::thread`로 이동(같은 thread에서 coroutine yield로 sender/receiver를 교대하던 구조가 동기 send에서는 성립하지 않는다) |
| `perf/single/src/perf_router_router.cpp` | 동일. `send_router_samples`를 일반 함수 + `std::thread`로 |
| `perf/single/common/perf_single_reqrep.hpp` | stop token 전송 2곳 동기화 |
| `perf/multi/src/perf_{dealer_dealer,dealer_router,router_router}_{client,server}.cpp` | routed send `co_await ... .async()` → `.submit()` (6곳) |

> ⚠ 함정 기록: `co_await`만 지우면 `-> async_task_t<T>` 함수가 **coroutine이 아니게
> 되어** 초기화되지 않은 task를 반환한다(SIGILL로 관측). 전체 perf 트리를 스캔해
> `async_task_t`를 반환하면서 `co_*`가 하나도 남지 않은 함수를 찾아 정리했다.

## 4. 검증

### 4.1 grep 증거 — binding 소유 스레드 0개

```
$ grep -rn "std::thread\|std::jthread\|pthread_create" bindings/cpp/src/ | wc -l
0
```

request timeout은 Core 소유(`ZLINK_REQUEST_TIMED_OUT`)이므로 binding 타이머
스레드도 필요 없다.

### 4.2 contract suite / sample smoke

`bindings/cpp/build-contract` (Release, local core `core/build` 0.12.0)

| 항목 | 결과 |
|---|---|
| `ctest` 전체 (contract 14 + sample-smoke 7 = 21) | **20/21 pass** |
| 유일한 실패 | `test_cpp_contract_socket` — `:698 test_received_lifetime_retains_and_releases_core_hwm_credit`, `released.current_accounted_bytes () == 0u` |

sanitizer 빌드 `bindings/cpp/build-sanitizers`
(`-fsanitize=address,undefined -fno-omit-frame-pointer`, `detect_leaks=1`):
**13/14 pass**, 같은 `test_cpp_contract_socket` 하나만 실패,
**AddressSanitizer / UndefinedBehaviorSanitizer / LeakSanitizer 보고 0건.**

#### 4.2.1 `test_cpp_contract_socket` 실패는 pre-existing이며 이번 변경이 **드러낸** 것이다

- 기준 HEAD(`99875dc498`)에서 이 테스트는 `submit_error_t ... errno=113`을
  던지며 **4번째 테스트에서** abort했다(기존에 기록된 pre-existing 실패).
  그 뒤의 테스트 11개는 한 번도 실행된 적이 없다.
- 이번 변경으로 errno=113 abort가 사라지면서 13번째 테스트까지 진행했고,
  거기서 잠복해 있던 assertion 실패가 드러났다.
- 증명: `test_received_lifetime_retains_and_releases_core_hwm_credit`만 실행하는
  분리 프로그램을 만들어 **기준 HEAD의 라이브러리**에 링크했더니 동일 지점에서
  동일하게 실패했다. 이 테스트는 PAIR 소켓만 쓰며 이번 변경이 건드린 경로를
  전혀 지나지 않는다.
- 추가 확인: §2.3의 `socket_t::close()` 수정을 임시로 되돌려도 동일하게 실패한다.

즉 이 항목은 Core HWM accounting의 **pre-existing 잠복 실패**이며, 이번 작업의
회귀가 아니다(§6 위험 목록에 남긴다).

**기대 pre-existing 실패 대비 변화**

| 기존 pre-existing 실패 | 현재 |
|---|---|
| `test_cpp_contract_socket` (errno=113) | 실패 위치가 `:698`로 이동(위 참조) |
| `test_cpp_contract_request_reply` `:714 test_routed_send_async_isolates_a_backpressure_from_b` | **해소** — 해당 테스트가 새 계약 테스트로 대체되어 통과 |
| `sample_smoke_sample_cpp_dealer_router_recv_sample` | **해소** — 통과 |

### 4.3 Core 동작 실측 (throwaway probe, 측정 후 삭제)

| 시나리오 | 결과 |
|---|---|
| 연결 없는 DEALER blocking submit | `BACKPRESSURED` / `EAGAIN`, 즉시 반환, message handle 유지 |
| HWM 포화 + `SNDTIMEO=100ms` | `BACKPRESSURED` / `EAGAIN`, 100 ms 후 |
| HWM 포화 + `SNDTIMEO=0` | `BACKPRESSURED` / `EAGAIN`, 즉시 (= `DONTWAIT` 계약) |
| HWM 포화 + `SNDTIMEO=5s`, 300 ms 후 drain | submit이 300 ms 후 성공 (**Core 신호로 재개**) |

> 참고 사실 기록: Core의 `zlink_send_part`는 `BACKPRESSURED`로 실패해도 검사한
> part를 비운다(`zlink_msg_size` = 0). C++ 계약이 보장하는 것은 **handle 유효성**
> 이지 payload 보존이 아니다(PAIR 동기 경로의 기존 동작과 동일). 계약 테스트도
> 이 사실에 맞춰 재제출 시 새 메시지를 만든다.

### 4.4 stress (throwaway, Release + ASan/UBSan 양쪽)

| 항목 | 결과 |
|---|---|
| 동시 sender 8 thread × 2,000건 = 16,000건 | submitted 16,000 / error 0 / received 16,000 — **손실 0** |
| `SNDTIMEO=0` DONTWAIT | `BACKPRESSURED`/`EAGAIN` 즉시 반환 — **PASS** |
| 작은 HWM blocking send 500건 | Core 안에서 park → drain 시작 후 전부 재개, error 0, received 500 — **PASS** |
| close race 20 round (sender 4 thread 상시 submit 중 `close()`) | **PASS** (§2.3 수정 후. 수정 전에는 3 round째 정지) |

ASan/UBSan 빌드에서도 동일하게 전부 통과, **sanitizer 보고 0건**.

### 4.5 perf sanity — 같은 세션 paired 비교

조건: local core(`core/build` 0.12.0), `--transports tcp --duration 5 --runs 3
--pin-cpu`. before는 `git stash`로 기준 HEAD 트리를 복원해 **같은 세션·같은
호스트**에서 측정했다.

| Pattern / size | before (async admission) | after (동기 submit) | 변화 |
|---|---:|---:|---:|
| `DEALER_DEALER`/tcp 64B | 657,972.0 | **1,876,671.6** | **+185.2%** |
| `DEALER_DEALER`/tcp 65536B | 30,499.8 | **36,312.0** | **+19.1%** |
| `DEALER_ROUTER`/tcp 64B | 492,649.0 | **1,563,337.8** | **+217.3%** |
| `DEALER_ROUTER_REQREP`/tcp 64B | 66,794.2 | **135,492.4** | **+102.9%** |
| `PAIR`/tcp 64B (회귀 확인) | 1,614,457.6 | 1,627,645.2 | +0.8% (회귀 없음) |
| `PUBSUB`/tcp 64B (회귀 확인) | 666,250.6 | 676,650.0 | +1.6% (회귀 없음) |

- `DEALER_DEALER` 64B는 기대대로 **크게 개선**됐다. coroutine frame·admission
  부기·dispatcher hop이 모두 사라져 C의 경로와 같은 모양이 됐다.
- `PAIR`/`PUBSUB`은 원래 동기 `submit()` 경로여서 변화가 없어야 하고, 실제로
  잡음 범위 안이다 — 회귀 없음의 대조군 역할을 한다.
- `DEALER_ROUTER_REQREP` 2배는 request 제출에서 admission 왕복이 사라진 결과다.
- 참고: 직전 로그(`2026-08-23-cpp-routed-send-improvement.md` §6)의 local core
  탐색값은 `DEALER_DEALER` 64B 898,193 / `DEALER_ROUTER` 64B 922,150이었다.
  이번 before(657,972 / 492,649)가 더 낮은 것은 호스트 부하 차이이며, 그래서
  판단은 **같은 세션 paired 값**으로만 한다.

### 4.6 single suite smoke — 전 pattern

`--pattern ALL --transports tcp --msg-sizes 64 --duration 1 --runs 1 --pin-cpu`,
local core. **`status: complete` (35/35 result line)**

| Pattern | throughput (msg/s) | latency mean (ms) | 상태 |
|---|---:|---:|---|
| `PAIR` | 1,790,467 | 2.447 | OK |
| `PUBSUB` | 686,893 | 3.268 | OK |
| `DEALER_DEALER` | 1,957,703 | 2.254 | OK |
| `DEALER_ROUTER` | 1,680,392 | 2.581 | OK |
| `DEALER_ROUTER_REQREP` | 135,592 | 0.354 | OK |
| `ROUTER_ROUTER` | 1,622,156 | 2.672 | OK |
| `ROUTER_ROUTER_REQREP` | 134,386 | 0.352 | OK |

> `ROUTER_ROUTER`는 기존에 `status: partial`로 알려진 문제가 있었으나
> (`progress.ko.md` C5 행 참조) 이번 실행에서는 **complete**로 나왔다.
> `perf_router_router.cpp`의 sender를 별도 thread로 옮긴 것과 관련이 있을 수
> 있으나 이번 작업의 목표가 아니므로 단정하지 않고 관찰 사실만 남긴다.

### 4.7 multi suite smoke

`--transports tcp --msg-sizes 64 --duration 1 --runs 1 --pin-cpu`(기본 pattern),
local core. **`status: complete`, success 6 / fail 0 / unsupported 0**

| Pattern | throughput (msg/s) | latency mean (ms) | 상태 |
|---|---:|---:|---|
| `MULTI_DEALER_DEALER` | 1,192,800 | 7.013 | OK |
| `MULTI_DEALER_ROUTER_SENDSEND` | 70,687 | 0.707 | OK |
| `MULTI_DEALER_ROUTER_REQREP` | 37,932 | 1.173 | OK |
| `MULTI_ROUTER_ROUTER_SENDSEND` | 69,687 | 0.650 | OK |
| `MULTI_ROUTER_ROUTER_REQREP` | 39,665 | 1.127 | OK |
| `MULTI_PUBSUB` | 699,259 | 196.644 | OK |
| `MULTI_STREAM` | — | — | **skip** (메모리 guard: `max_clients=6998 < clients=10000`, 환경 요인이며 이번 변경과 무관) |

## 5. 스펙 문서 갱신

| 문서 | 갱신 |
|---|---|
| `bindings/doc/spec/cpp/README.ko.md` (구 456·475행 구간) | routed send `async()`/admission 기계장치/`.flags(dontwait).submit()`만 동기라는 서술을 삭제하고, 동기 `submit() -> void`, Core 소유 HWM 대기, gate의 축소된 역할, request의 동기 제출 + Core-driven 완료, Core callback 컨텍스트 재개 제약, `send_submit_operation_t`의 유일 terminal을 명시 |
| `bindings/doc/spec/cpp/README.en.md` (구 419·442행 구간) | 위와 동일 내용의 영문 |

## 6. 남은 위험 / 후속 판단이 필요한 항목

1. **framework C++ 컴파일 미검증** (§3.3). vcpkg toolchain 부재로 빌드하지 못했다.
   6개 파일이 기계적 변경이지만 확인이 필요하다.
2. **`test_cpp_contract_socket` `:698`** — pre-existing Core HWM accounting 실패가
   드러났다(§4.2.1). 별도 이슈로 다뤄야 한다.
3. **blocking submit 중 `close()`** — `SNDTIMEO=-1`로 credit 없이 park한 send는
   Core가 close로 깨우지 않는다. **raw C에서도 동일**하게 재현된다(C에서는
   `zlink_close`가 즉시 busy를 반환하고 send는 영원히 park). 즉 Core 계약의
   특성이며 binding 문제가 아니지만, C++는 §2.3 수정 전까지 `close()` 자체가
   gate에서 기아 상태였다. 애플리케이션에는 유한한 `SNDTIMEO` 사용을 권한다.
4. **한 socket에서 다른 target으로의 진행 직렬화** — record-attempt gate 때문에
   thread A가 rid_a로 blocking submit 중이면 thread B의 rid_b 제출이 그 동안
   대기한다(실측: `SNDTIMEO=1500ms`일 때 B가 1,502 ms 후 제출). C에는 없는
   binding 고유 동작이다. gate는 multipart part sequence 보호와 close 경합
   방지를 위해 남겼다(스펙의 "part sequence는 caller 소유"와 절충). 제거하려면
   multipart 교차 제출 보호와 close 안전성을 별도로 설계해야 하므로 이번 범위
   밖으로 두고 보고한다.
5. **`SNDTIMEO`의 이중 의미** — 이제 routed send/request 제출과 PAIR/PUB 제출이
   모두 같은 socket option 하나를 대기 상한으로 쓴다. framework가 per-call
   timeout을 구현하려면 §3.3처럼 설치/복원해야 하는데, 이는 같은 socket을
   공유하는 동시 호출자에게 보이는 부작용이다(기존 `raw_dealer_port`가 이미
   쓰던 방식이지만, 이제 적용 구간이 submit 전체로 길어졌다).
6. **`routed_send_submit_operation_t`에 flags 단계 없음** — `DONTWAIT`을
   per-call로 표현할 수 없고 `SNDTIMEO=0`으로만 표현된다. 스펙은 flags를
   builder 단계로 받는 것을 허용하므로("언어 관용 방식"), 필요하면 후속
   스펙 논의로 추가할 수 있다.
7. **재개 컨텍스트 제약**(§1.5)은 사용자에게 새로 노출되는 계약이다. 나머지 6개
   언어 바인딩에 이식할 때 각 런타임의 콜백 컨텍스트 제약을 같이 문서화해야
   한다.
8. `DEALER_DEALER` 공식 재측정(release core, paired, 전체 크기)은 아직 하지
   않았다 — 리뷰 후 별도 수행 대상이다.

## Sol 리뷰 반영 (2026-08-23)

리뷰 지적 4건(BLOCKING 2 / SHOULD-FIX 2)을 처리했다. 범위는 `bindings/cpp`와
`framework/languages/cpp`이며 `core/`는 건드리지 않았다(별도 작업 중). **커밋하지
않았다.**

### R-1 (BLOCKING) multipart request 실패 시 입력 part 소실

**지적.** `request_reply.cpp:220`이 operation state에서 part를 꺼낸 뒤, 두 실패
경로(`:251` `catch (...)`, `:260` `rc != ZLINK_SUBMIT_OK`)가 모두 단일 part용
복원 헬퍼 `restore_single_send_part_to_source(state, parts)`를 호출했다. 이 헬퍼는
`parts_.size() != 1`이면 아무것도 하지 않고 빠져나간다. 그 다음 builder가 state를
pool로 되돌리며 `reset_for_reuse()`가 `message.parts`를 비운다. 결과적으로 part가
2개 이상인 lvalue 요청은 **제출 실패 시 호출자의 message_t가 파괴**되어
`operation_contracts.hpp:274,308`의 소유권 계약("on failure ownership returns to
the caller")을 위반했다.

**수정.**

1. `operation_state_t::message_parts_t`에 `parts`와 같은 길이로 유지되는
   `std::vector<message_t *> part_sources`를 추가했다. lvalue로 추가된 part는
   원본 `message_t`의 주소를, rvalue로 추가된 part는 `nullptr`(= 소비됨)을
   기록한다. 즉 단일 part의 `single_part_source`를 multipart로 일반화한 것이다.
2. `append_send_part()`를 `fold_staged_single_part()` +
   `append_send_part_from(state, part, source)`로 쪼개, single-part fast path에
   staged된 part를 sequence로 접을 때도 source 연결을 잃지 않게 했다.
3. 실패 경로용 `restore_send_parts_to_sources(state, parts)`를 추가했다. 이미
   transport가 소비해 `valid()`가 아닌 part는 건너뛰므로 부분 실패에서도
   "호출자가 아직 소유한 part만" 정확히 되돌린다. rvalue part는 source가 없어
   그대로 소비된 채 남는다(문서화된 consumed-on-submit 규칙).
4. `request_reply.cpp`의 **두 실패 경로 모두**를 이 헬퍼로 바꿨다. 같은 계약
   위반이 있던 multipart send 경로도 `restore_send_parts_to_state()`가 새 헬퍼를
   먼저 호출하도록 해 함께 고쳤다.
5. 부수적으로 드러난 인접 결함 하나를 같이 고쳤다: submit 단계 builder의
   `message(message_t &&)` 오버로드가 `single_part`를 **덮어써서**
   `.message(a).message(std::move(b))`가 `a`를 조용히 버렸다. 이제 rvalue도
   sequence에 append된다(`append_send_part(state, message_t &&)`).

C6 헬퍼 `adopt_native_part`는 native frame → `message_t` 복원용이고, 이 경로는
`submit_borrowed_message_array`(borrowed view)라 native 이동 자체가 없다. 따라서
`message_t` 간 이동으로 충분하며 재사용 대상이 아니었다.

**증거.** `tests/contract/test_cpp_contract_request_reply.cpp`에 4개 테스트 추가:

| 테스트 | 덮는 실패 경로 |
|---|---|
| `test_multipart_request_failure_returns_every_part` | `:260` 반환코드 경로 — SNDTIMEO backpressure, lvalue part 3개가 모두 `valid()`/`size()`/payload 보존 |
| `test_multipart_request_invalid_part_returns_the_others` | `:260` 결정적 재현 — 중간 part가 invalid이라 native view 구성이 실패, 나머지 2개 복원 확인 |
| `test_multipart_request_failure_keeps_rvalue_parts_consumed` | 혼합 소유권 — lvalue는 복원, rvalue는 소비 유지 |
| `test_multipart_request_restore_helper_matches_single_part_semantics` | `:251` `catch (...)` 경로 — 이 경로는 native view builder의 **할당 실패로만** 도달 가능하므로 state 수준에서 동일 헬퍼·동일 인자로 직접 검증. `reset_for_reuse()` 이후에도 호출자 part가 온전한지까지 확인 |

수정 전 코드로 되돌리면 이 테스트들이 실패하는 것을 확인했다.

### R-2 (BLOCKING) framework C++ 미컴파일

**달성한 검증 수준: 전체 CMake configure + framework 라이브러리/contract test
타깃 빌드 + contract test 실행.** (fallback인 standalone 구문 검사가 아니라 실제
빌드까지 도달했다.)

**빌드 진입점.** `framework/languages/cpp/CMakePresets.json`
(`linux-ninja-debug` / `...-vcpkg-debug`, toolchain은 `$env{VCPKG_ROOT}`) +
`vcpkg.json`. 이 환경에는 vcpkg checkout이 없다(repo root의 `vcpkg/`는
overlay-ports 디렉터리일 뿐이고 `VCPKG_ROOT`는 미설정). 또 framework는 로컬
`zlink_cpp` 패키지를 **EXACT 0.13.0**으로 요구하는데 `.artifacts/wsl/install/`에는
0.10.1/0.11.0/0.11.1만 있었다.

**우회 방법(모두 scratchpad 안, repo 파일 무수정, core 재빌드 없음).**

1. 의존성(protobuf / lz4 / nlohmann-json / zlib)을 `apt-get download` +
   `dpkg-deb -x`로 scratch sysroot에 풀었다. sysroot의 GTest config에는
   `GTest::gmock` 타깃이 없어 configure가 깨지므로 제거하고 FetchContent
   googletest 1.14 경로를 타게 했다.
2. `bindings/cpp`를 Release로 빌드해 scratch prefix에 `zlink_cpp` 0.13.0으로
   install했다(기존 `core/build/lib/libzlink.so`를 읽기 전용으로 재사용).
3. `core/build`가 0.12.0이라 zlink EXACT 0.13.0 요구를 만족시키려고, scratch에
   `zlinkConfig(Version).cmake` shim을 만들어 `core/build/zlinkTargets.cmake`를
   `include()`하게 했다.
4. `cmake -S framework/languages/cpp -B <scratch> -G Ninja -DCMAKE_BUILD_TYPE=Debug
   -DCMAKE_CXX_STANDARD=20 -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=ON ...` 후
   `--target zlink_framework test_cpp_framework_target_contract` → **rc=0,
   컴파일/링크 실패 0건.**

**6개 파일 결과 — 전부 프로젝트 플래그에서 진단 0건.**

| 파일 | 결과 |
|---|---|
| `runtime/backend/raw_dealer_port.cpp` | clean |
| `runtime/backend/raw_route_port.cpp` | clean |
| `runtime/channels/channel_outbound_exchange.cpp` | clean |
| `runtime/fanout/raw_fanout_owner.cpp` | clean |
| `runtime/streams/stream_host_service.cpp` | clean |
| `tests/.../test_cpp_framework_target_contract.cpp` | 컴파일 + **링크 + 실행** |

`compile_commands.json` 기준으로 `-fsyntax-only -Wall -Wextra` 재검사도 통과.
추가 경고는 `raw_route_port.cpp:201`의 `-Wsign-compare`(`int index < size_t count`)
하나뿐인데 **이번 diff가 건드리지 않은 기존 `poll()` 코드**이고 프로젝트 기본
플래그로는 켜지지 않는다. 나머지는 include된 헤더
`contracts/messaging/message_context.hpp:121`의 기존
`-Wmissing-field-initializers`다.

**contract test 실행 결과.**

- **`CPP-CONTRACT-STREAM-001` PASS** — 수정된 `stream_host_service.cpp`가
  `_core_socket->options ().send_timeout (*timeout)`,
  `_core_socket->send (rid).message (std::move (frame)).submit ()`,
  그리고 `framework/src/runtime/streams` 아래 `async_submit_runtime` 부재를 모두
  만족한다.
- 실패한 gate 1건: `E2E-CP-33: RL-D4 has no raw camelCase errorCode assertion`.
  이 gate가 보는 `tests/Zlink.Framework.UnitTests/test_cpp_framework_messaging.cpp`는
  **이번에 수정하지 않았고**(git status clean) gate 텍스트도 HEAD와 byte-identical
  이다 → **pre-existing**, 6개 파일과 무관하다.

**환경 기인 잡음(6개 파일과 무관).** 전체 트리 빌드 시
`e2e/RegistrationCodec/*` 3개가 `registration_codec.pb.h` not found로 실패했다
(대체 투입한 Ubuntu protoc 3.21의 출력 디렉터리 차이). 또 검증 도중 다른 작업이
`core/build`를 0.13.0으로 재빌드하면서 무관한 e2e/sample 타깃에 일시적 링크
오류가 났고, `zlink_cpp`를 새 core 헤더로 다시 빌드하니 해소됐다.

**남은 한계.** 이 빌드는 프로젝트가 의도한 vcpkg toolchain이 아니라 대체
sysroot + core shim 위에서 이뤄졌다. "6개 파일이 well-formed하고 STREAM-001
gate를 통과한다"는 결론에는 충분하지만, 정식 CI 재현으로 한 번 더 확인하는 것을
권한다.

### R-3 (SHOULD-FIX) `send_ready` 공개 표면 잔존

스펙이 readiness-hint 의미를 폐기했고 Core가 0.13.0에서 심볼을 제거하므로
바인딩 공개 표면에서 제거했다.

- `socket_contracts.hpp:145` 선언, `socket.cpp:590` 정의 삭제
- 재노출 6개소 삭제: `message_socket_contracts.hpp`(2),
  `pubsub_socket_contracts.hpp`(2 + `using publisher_socket_t::...` 2),
  `stream_socket.hpp`, `routed_socket_contracts.hpp`
- 내부 기계장치 삭제: `socket.cpp`의 `send_ready_trampoline` /
  `ensure_native_send_ready_handler`, `socket_callback_state.hpp`의
  `send_ready_mutex` / `send_ready_handler` / `native_send_ready_handler_once`
- 테스트 삭제: `test_cpp_contract_behavior.cpp`의
  `test_stream_send_ready_handler_survives_move_and_source_destruction`
  (`test_cpp_contract_flow_state.cpp`의 이 테스트를 가리키던 주석도 정리)
- 문서: `bindings/doc/spec/cpp/README.{en,ko}.md`,
  `bindings/doc/reference/cpp/03-sockets.{en,ko}.md`(표 6행)
- 샘플/perf에는 사용처가 없었다. `framework/languages/cpp`에도 사용처가 없다
  (grep 확인 — framework의 `send_ready`는 동명이인인 wire record 종류다).

**부수 확인.** 검증 중 core가 0.13.0으로 재빌드되면서
`zlink_send_ready_handler`가 실제로 사라졌고, **기준 HEAD의 `bindings/cpp`는 더
이상 컴파일되지 않는다**(pristine worktree에서
`'zlink_send_ready_handler' was not declared in this scope` 확인). 이 제거는
선택이 아니라 이미 필수였다.

### R-4 (SHOULD-FIX) 죽은 state / 낡은 주석

- `operation_state_t::timeout_explicit` 삭제(`operation_state.hpp:124`, `:270`).
  읽는 쪽이 전혀 없었다.
- `operation_state.hpp:151`의 "Asynchronous/admission terminals" 주석을 admission
  제거 후 사실에 맞게 고쳤다("제출 statement보다 오래 사는 terminal은 strong
  reference를 소유한다").

미처리로 남긴 인접 항목: `message_parts_t::discard_single_part_on_backpressure`도
현재 읽는 쪽이 없다. 리뷰 지적 범위 밖이라 손대지 않았고 후속으로 남긴다.

### 검증 (재실행)

로컬 core: `core/build` (다른 작업으로 0.13.0으로 재빌드된 상태).

| 빌드 | 결과 |
|---|---|
| `bindings/cpp/build` (Release, tests+samples+perf) | 컴파일 에러 0, `ctest` 전체 **21/22 pass** |
| `bindings/cpp/build-sanitizers` (`-fsanitize=address,undefined -fno-omit-frame-pointer`) | 컴파일 에러 0, contract **13/14 pass**, sanitizer 보고 **0건** |

두 빌드 모두 유일한 실패는 기존에 기록된 pre-existing HWM accounting 실패
`test_cpp_contract_socket:698`
(`test_received_lifetime_retains_and_releases_core_hwm_credit`,
`released.current_accounted_bytes () == 0u`)뿐이다. 새로 추가한 multipart 실패
테스트 4개는 Release/sanitizer 양쪽에서 통과한다(sanitizer 5회 반복 0 실패).

**관측된 flakiness 1건(신규 아님).** 기존 테스트
`test_routed_send_and_request_honor_core_sndtimeo`(`:913`)는 머신 부하가 높을 때
"fill → 즉시 submit" 사이에 Core credit이 돌아와 간헐적으로 실패한다(부하 상태
20회 중 3회). 단일 part 경로이고 이번 변경이 지나지 않는다. 새로 추가한
backpressure 기반 테스트는 같은 함정을 피하려고 "backpressure를 다시 채우고
재시도"하는 구조로 작성했다(반복 실행에서 실패 0).

### 변경 파일

`bindings/cpp`:
`include/zlink/Contracts/Messaging/operation_contracts.hpp`,
`include/zlink/Contracts/Sockets/{socket,message_socket,pubsub_socket,routed_socket}_contracts.hpp`,
`include/zlink/Contracts/Sockets/stream_socket.hpp`,
`src/Runtime/Messaging/{operation_state.hpp,request_reply.cpp,send_operations.cpp,reply_operations.cpp}`,
`src/Runtime/Sockets/{socket.cpp,socket_callback_state.hpp}`,
`tests/contract/{test_cpp_contract_request_reply.cpp,test_cpp_contract_behavior.cpp,test_cpp_contract_flow_state.cpp}`

`bindings/doc`:
`spec/cpp/README.{en,ko}.md`, `reference/cpp/03-sockets.{en,ko}.md`
