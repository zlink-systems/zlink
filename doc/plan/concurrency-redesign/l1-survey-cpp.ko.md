# L1 표본 조사 — cpp (codex sol, 2026-08-26)

> 감독: Claude. codex 조사 최종 보고 전문이다. L1 전환 요청의 근거로 보존한다.

# 결론

C++의 직접 대응 정본은 [`stream_session_registry_t`](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stream_session_registry.hpp:117)이다. .NET 표본의 binding, generation fence, ingress drain, relocation seal, remote tenure, retained outbound 책임이 가장 정확히 일치한다.

다만 actor-side projection이 [`actor_gateway_state_t`](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.hpp:179)와 [`session_actor_binding_context_t`](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.hpp:144)에 분산되어 있다. 세 클래스 모두 스펙상 **C2**다. 클래스 하나와 기존 gate 하나를 전환 단위로 삼으라는 규칙상, 한 번에 합치거나 경계를 재설계하면 안 된다([스펙 §8](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:167)).

### `stream_session_registry_t`

[`stream_session_registry.hpp`](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stream_session_registry.hpp:117), [`stream_session_registry.cpp`](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stream_session_registry.cpp:42)

- lock **31개**, 전부 단일 `std::mutex _mutex`. 별도 동기화 수단은 `std::condition_variable _changed` 1개다([필드](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stream_session_registry.hpp:285)).
- 보호 상태:
  - `_connections`와 그 내부 `connection_state_t::bindings`
  - `_last_connection_generation`
  - `_actor_bindings`
  - `_barriers`
  - binding generation/barrier token counter, `_all_sealed`
  - binding별 inbound sequence/drain, barrier, pending tenure, retained outbound queue, route-publish flag([aggregate](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stream_session_registry.hpp:238)).
- 여러 컬렉션 동시 접근: helper와 중첩 `bindings`까지 의미적으로 펼치면 **29/31**. 최상위 map을 같은 본문에서 직접 언급한 블록만 세면 **11/31**이다. 전자는 실제 불변식 기준 수치다.
- 판정: **C2**. connection→actor index→binding aggregate와 barrier/tenure/drain을 함께 전이하며, 결정 뒤 callback·전송 작업이 이어진다. C2가 여러 field/collection 불변식에 해당한다는 기준과 직접 일치한다([스펙 §4](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:80)).
- 파급: 생산 코드 **4파일/38지점** — operation 호출 37곳과 생성 1곳. 호출 포함 루틴 기준 async/coroutine **5/15, 약 33%**다. 호출점 가중으로는 **20/37, 약 54%**가 coroutine 또는 비동기 continuation 문맥이다.
- 재진입 의심: 동일 `_mutex`의 중첩 재획득은 **0곳 확인**. `current_aggregate_unlocked()` 계열은 lock을 다시 잡지 않으며, resolver와 terminal/close callback도 unlock 뒤 호출한다. 다만 [`admit_inbound()`의 조건변수 대기](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stream_session_registry.cpp:400)는 lane 전환 시 blocking 경계를 별도로 제거해야 한다.
- lane 밖 snapshot/참조:
  - `stream_dispatch_t`가 binding 값과 `shared_ptr<stream_ingress_drain_t>`를 반환한다([정의](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stream_session_registry.hpp:49)). 이 `shared_ptr`는 connection/binding이 제거된 뒤에도 drain을 살려 둔다.
  - close/replace는 기존 drain의 `accepts_completion=false`를 먼저 기록한다. 따라서 수명은 안전하게 연장되지만, lane 밖에 **mutable capability**가 남는다.
  - `current_binding()`은 값 복사 snapshot이며 이후 stale할 수 있으므로 generation/connection fence가 필수다([구현](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stream_session_registry.cpp:971)).
  - retained completion과 close callback은 컨테이너에서 move한 뒤 unlock 후 호출한다([예](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stream_session_registry.cpp:615)).
- 장기 작업: 자체 timer/background loop 시작은 **0개**. 단, bounded `condition_variable::wait_until` 1곳이 있다. 외부 장기 경계는 `stream_host_service`의 bind/retry coroutine과 retire loop다([bind 시작](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/streams/stream_host_service.cpp:1946), [retire 시작](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/streams/stream_host_service.cpp:2325)).
- POSDDD:
  - 모든 session/connection이 단일 mutex를 공유해 경합 범위가 넓다.
  - `_changed.notify_all()`은 waiter가 늘면 불필요한 wake-up을 만든다.
  - binding aggregate마다 `shared_ptr` drain을 할당하고, retained/discarded callback vector를 반복 materialize한다.
  - `current_binding`, admission 결과 및 route 전환에서 문자열을 포함한 binding/proof 값 복사가 발생한다.
  - callback을 lock 밖으로 move해 실행하는 현재 구조는 유지할 가치가 있다.

### `actor_gateway_state_t`

[`actor_gateway_runtime.hpp`](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.hpp:179), [`actor_gateway_runtime.cpp`](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:163)

- lock **71개**, 전부 단일 **`std::recursive_mutex`**다([선언](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.hpp:182)).
- 보호 상태: actor record map, bound-session sink/replacement-handler map, relay/push vector, pending/active relay와 send queue/set, join-delivery fence map, relay-completion set, dispatcher/callback, offload flag, serializer/dispatch 설정과 binding-token counter([필드](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.hpp:221)).
- 여러 컬렉션 직접 접근: **21/71**.
- 판정: **C2**. actor record, sink, route fence, pending/active queue가 하나의 session binding·delivery 불변식을 구성한다.
- 파급:
  - opaque `actor_gateway_state_t` 직접 소유는 구현 header/cpp **2파일**에 국한된다.
  - `actor_gateway_runtime_t` facade까지 포함한 생산 코드 파급은 **5파일/75지점**이다.
  - C++ coroutine/task/callback 관용구를 포함한 caller 비율은 정적 근삿값으로 **약 1/3**이다. 중요한 점은 lock 내부 suspension은 **0곳**이고, task/callback은 snapshot 후 lock 밖에서 실행된다는 것이다.
- 재진입 증거:
  - manager completion 후 같은 manager의 `find()`와 `bind_or_get()`을 다시 호출하는 실제 회귀 시험이 있다([L396–400](/home/hep7/project/zlink/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_gateway.cpp:396)).
  - 그러나 현재 소스에서 **동일 `recursive_mutex`를 잡은 채 다시 잡는 중첩 재획득은 0곳 확인**됐다. callback도 복사 후 unlock 뒤 호출한다([membership 예](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:818)).
  - 따라서 `recursive_mutex`와 논리적 async 재진입은 실재하지만, 이것만으로 재귀 mutex가 필수라는 증거는 아니다. `fast_mutex` 전환 시 즉시 abort시킬 후보이되, 실제 abort가 나면 그 스택을 재진입 정본으로 삼아야 한다.
- lane 밖 snapshot/참조:
  - runtime, manager, actor context, bound-session 객체가 동일 `_state`를 `shared_ptr`로 보유해 owner facade보다 상태 수명이 길어질 수 있다.
  - bound-session sink를 lock 아래 복사한 뒤 unlock 후 호출한다([L665–720](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:665)). retire/unregister 이후에도 이미 복사된 sink는 한 번 실행될 수 있으므로 generation/route fence가 의미적 안전장치다.
  - `actor_session_binding_snapshot_t`는 actor record 값과 sink `shared_ptr`를 함께 반출한다([정의](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.hpp:138)). async native bind 실패 뒤 복구에 쓰이므로 stale restoration fence가 필요하다.
  - replacement-handler vector도 `shared_ptr` snapshot으로 복사된 뒤 lock 밖에서 실행된다.
- 장기 작업 시작점:
  - detached bound-session send drain과 task observer([L163–220](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:163))
  - session-relay drain coroutine
  - offloaded relay task
  - native binder await와 실패 compensation
  - 자체 timer/expiry loop는 없다.
- POSDDD:
  - 71개 임계 구역이 하나의 recursive mutex를 공유한다.
  - sink/fence/task용 `shared_ptr` 할당과 handler-vector snapshot 복사가 반복된다.
  - `bound_session_pushes`는 payload frame을 복사해 축적한다([L705–706](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:705)).
  - sink/handler를 lock 밖에서 호출하는 현재 분리는 경합과 continuation deadlock 방지 측면에서 보존해야 한다.

### `session_actor_binding_context_t`

[`actor_gateway_runtime.hpp`](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.hpp:144)

- lock **7개**, 단일 `std::mutex`.
- 보호 상태: stream/stream state/session ID/codec, `actor_tokens`, `ready_actors`, `actor_streams`, native binder.
- 여러 컬렉션 동시 접근: **2/7**.
- 판정: **C2**. token, readiness, weak stream projection을 함께 갱신한다.
- 파급은 actor manager 내부에 갇혀 있으며, lock-bearing 루틴 중 task/coroutine은 **1/7, 약 14%**다.
- 재진입 의심: 동일 mutex 중첩은 확인되지 않았다. 다만 binding-context mutex를 잡은 뒤 actor-gateway mutex로 들어가는 lock 순서가 있다([L1660](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:1660)). native binder는 binding mutex를 해제한 뒤 await한다.
- lane 밖 참조: `stream_state`는 `shared_ptr`, `actor_streams`는 `weak_ptr`; native binder와 stream snapshot이 비동기 경계를 넘는다.
- POSDDD: 두 mutex 사이 순서 추론 비용과 token/map 중복 조회가 주요 관찰점이다.

## 예상 난이도

- `stream_session_registry_t` 단독: .NET 표본 대비 **약 1.5–2배**.
- actor-side projection까지 L1 동등 책임을 모두 전환: **약 3–4배**, 중심 추정 **3.5배**.
- 근거: 30 lock/35 호출점이던 .NET 표본([기준](/home/hep7/project/zlink/doc/plan/concurrency-redesign/handoff.ko.md:108))에 비해 C++ 전체는 3개 gate, 109개 lock acquisition, `recursive_mutex`, blocking wait, 다수의 `shared_ptr` mutable snapshot과 비동기 compensation 경계를 가진다.

조사 시간은 **15분 29초**(2026-08-26 15:10:39–15:26:08 KST)였다. 변경 파일은 없으며, 빌드·테스트·git 명령은 실행하지 않았다. 현재 브랜치는 git 금지 조건 때문에 확인하지 않고 사용자 제공값 `refactor/lane-ownership-concurrency`를 기준으로 했다.


