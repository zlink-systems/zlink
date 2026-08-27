# CP3 C++ 감사 — lock 경계 상태 소유권

## 1. 결론

**판정: NOT CLEAN.** 테스트를 제외한 C++ Framework 구현 266파일을 정적 감사한 결과,
lock RAII 최초 취득 886곳과 명시적 재취득 26곳, 합계 **912곳**을 확인했다. 이 중
실행 primitive 214곳, socket·dispose 프로토콜 230곳, 상태 보호 잔존 460곳, 기타 8곳이다.

lock을 해제한 뒤 callback/future/I/O/lane 경계로 넘기는 source는 **119곳**이다. **117곳은
정당화**할 수 있고, `channel_native_client_t`의 request/send **2곳은 [H] 결함 의심**이다.

- 전체 감사 발견: **[C] 0건, [H] 2건, [M] 1건, [L] 1건**. 이 중 async snapshot
  결함 의심은 **[H] 2건**이다.
- 보류 T7 2건: `channel_runtime_state_t`는 **[M] C2 상태 보호 미해소**, `mesh_node_runtime_t`는
  독립 protocol/C1 영역으로 **조건부 해소**
- `lane.run(...).get()` blocking bridge: **456곳**. 스펙 06 §5의 세 조건은 정적으로
  충족하지만, 동기 public 표면과 return-before 계약 때문에 남은 호환 부채다.
- **[L]** `progress.ko.md`의 “cpp mutex 취득 ~204”는 현재 소스와 다르다. explicit-template
  표기만 찾으면 204 token이 나오지만 CTAD와 명시적 재취득을 포함한 source 취득 위치는 912곳이다.
- POSDDD ②: 정적 우선순위 상위 10개를 §7에 기록했다. profile/benchmark 전이므로 성능
  결함으로 확정하지 않는다.

사용자가 지정한 `framework/languages/cpp/src`와 `framework/languages/cpp/include`는 현재
checkout에 없다. 실제 production 경로인 `framework/languages/cpp/framework/src`와
`framework/languages/cpp/framework/include`를 대상으로 삼았다. 이후 표의 `runtime/...`은
앞 경로의 `framework/src/runtime/...`, `include/...`은 `framework/include/...`을 뜻한다.
`core/src`와 test/tests 경로는 계수에서 제외했다.

## 2. 기준과 측정 방법

판정 기준은 공통 명세의 §3 stale snapshot 금지, §4 ownership-region 분류와 protocol gate,
§5 blocking bridge, §6 유형, §7 return-before, §9 언어별 동등물이다
(`framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md`).
발견 목록 1·2·4·5·6·7·8·9를 모두 적용했고, POSDDD 성능 원칙은
`doc/principal/dev/posddd.ko.md`의 “측정 우선, 불필요한 할당·복사·경합 후보 기록”을 적용했다.

### 2.1 lock 취득 계수

계수 단위는 **동적 횟수가 아니라 source의 취득 위치**다. `condition_variable::wait`가 내부에서
unlock/relock하는 동적 동작은 최초 `unique_lock` source 한 곳으로 센다. 재현 명령은 다음과 같다.

```bash
ROOT=framework/languages/cpp/framework

# 대상 파일 수: 266
rg --files "$ROOT/src" "$ROOT/include" \
  -g '*.cpp' -g '*.hpp' -g '*.cc' -g '*.cxx' -g '*.h' -g '*.hxx' \
  | rg -v '(^|/)(test|tests)(/|$)' | wc -l

# RAII lock token: lock_guard 832, unique_lock 56, shared_lock 2, scoped_lock 0
rg -o --no-filename --glob '*.{cpp,hpp,cc,cxx,h,hxx}' \
  '\bstd::(lock_guard|unique_lock|shared_lock|scoped_lock)\b' \
  "$ROOT/src" "$ROOT/include" | sort | uniq -c

# 명시적 재취득 후보와 비 RAII API 확인
rg -n --glob '*.{cpp,hpp,cc,cxx,h,hxx}' \
  '\.[[:space:]]*(lock|try_lock|try_lock_for|try_lock_until)[[:space:]]*\(|\bstd::lock[[:space:]]*\(' \
  "$ROOT/src" "$ROOT/include"
```

RAII token 890개 중 `unique_lock` type만 언급하고 취득하지 않는 4곳
(`runtime/spots/spot_runtime.hpp:1702,1721`, `spot_runtime.cpp:4449,9346`)을 제외해 886곳이다.
명시적 `.lock()` 26곳을 더했다. `try_lock*`, `std::lock`, `scoped_lock`은 0곳이다. `.lock()`
26곳은 `include/.../contracts/workers/worker.hpp:335`,
`runtime/actors/actor_gateway_runtime.cpp:1750,1758`,
`runtime/spots/spot_runtime.cpp:2617,2639,2697,2712,2752,2767,4472,4475,7602,7610,7613,7620,7628,9445,9463,9476,9481,9496,9500,9609,9645`,
`runtime/streams/stream_host_service.cpp:3152,3156`이다.

진행표의 약 204는 `std::lock_guard<...>` 187 token과 `std::unique_lock<...>` 17 token의
합과 일치한다. 이는 C++17 CTAD의 `std::lock_guard lock(mutex)` 형태를 놓치며, 17개 중에도
위 type-only 4개가 섞인다. 따라서 204는 현재 취득 실측으로 재사용할 수 없다.

분류는 lock type이 아니라 그 lock이 보호하는 ownership region을 기준으로 했다. 실행 queue,
permit, timer, exact-operation table은 “실행 primitive”, socket과 close/drain/active-count gate는
“socket·dispose 프로토콜”, mutable application/runtime registry는 “상태 보호 잔존”, ID 생성기와
host-capacity 같은 독립 보조 상태는 “기타”다. 한 파일에 여러 영역이 있으면 acquisition source별로
나눴다.

### 2.2 경계 snapshot seed 추출과 제외

1. 주석·문자열을 제거하고 괄호/중괄호 scope를 추적하는 정적 lexical scanner로 각 함수의
   RAII lock scope 종료 또는 `unique_lock::unlock()` 뒤에 callback 등록, `co_await`, future/result
   대기, executor/I/O submit, `lane.run`이 오는 후보를 뽑았다. 경로 비민감 seed는 **107곳**이었다.
2. lock 아래서 callback/boolean/task를 반환해 호출자가 경계 뒤 사용하는 escape를 수동 역추적해
   **12곳**을 추가했다. 최종 source는 **119곳**이다.
3. `weak_ptr::lock()`, 주석·문자열, type-only `unique_lock`, 경계보다 뒤에서 처음 취득하는 lock,
   lock 안에서 끝나는 동기 호출, immutable 설정만 읽는 곳은 제외했다. snapshot 뒤 exact-current
   재조회가 있는 곳은 제외하지 않고 “경계 뒤 재조회”로 정당화했다.
4. 같은 함수라도 서로 다른 lock source가 별도 future/I/O를 소유하면 별도 source로 셌다. 반대로
   한 source가 여러 경계를 지나도 한 번만 셌다. 각 source에서 exact identity, serial owner,
   lifecycle terminal, 경계 뒤 재조회 중 하나를 확인했고 어느 것도 없으면 결함 의심으로 판정했다.

이 방법은 C++ AST의 완전한 alias 분석이 아니라 재현 가능한 보수적 lexical seed와 수동
control-flow/호출자 추적의 결합이다. 숫자는 추정이 아니라 위 규칙으로 현재 checkout에서 센
source 위치 수이며, 동적 race 발생 빈도는 측정하지 않았다.

## 3. lock·동기화 취득 전수 계수

표의 네 숫자는 차례로 실행 primitive / socket·dispose 프로토콜 / 상태 보호 잔존 / 기타다.
행 합계는 912이며, acquisition이 없는 production 파일 206개는 생략했다.

| 파일 | 실행 | socket·dispose | 상태 보호 | 기타 | 합계 |
|---|---:|---:|---:|---:|---:|
| `include/zlink/framework/contracts/dispatch/task.hpp` | 6 | 0 | 0 | 0 | 6 |
| `include/zlink/framework/contracts/messaging/message.hpp` | 0 | 0 | 0 | 1 | 1 |
| `include/zlink/framework/contracts/workers/worker.hpp` | 3 | 0 | 0 | 0 | 3 |
| `runtime/actors/actor_client.cpp` | 0 | 0 | 9 | 0 | 9 |
| `runtime/actors/actor_gateway_runtime.cpp` | 3 | 0 | 9 | 0 | 12 |
| `runtime/backend/raw_dealer_port.cpp` | 0 | 5 | 0 | 0 | 5 |
| `runtime/backend/raw_route_port.cpp` | 0 | 6 | 0 | 0 | 6 |
| `runtime/channels/channel_host_service.cpp` | 3 | 0 | 0 | 0 | 3 |
| `runtime/channels/channel_outbound_exchange.cpp` | 0 | 30 | 0 | 0 | 30 |
| `runtime/channels/channel_runtime.cpp` | 0 | 0 | 37 | 0 | 37 |
| `runtime/channels/channel_runtime_bundle.cpp` | 0 | 6 | 0 | 0 | 6 |
| `runtime/channels/route_channel_runtime.cpp` | 0 | 2 | 0 | 0 | 2 |
| `runtime/client_server/client_server_location_runtime.cpp` | 0 | 7 | 0 | 0 | 7 |
| `runtime/codecs/serializer.cpp` | 0 | 0 | 2 | 0 | 2 |
| `runtime/configuration/endpoint_connections.cpp` | 0 | 0 | 7 | 0 | 7 |
| `runtime/diagnostics/listener_status_registry.hpp` | 0 | 0 | 3 | 0 | 3 |
| `runtime/diagnostics/logging.cpp` | 0 | 0 | 12 | 0 | 12 |
| `runtime/diagnostics/monitoring_runtime.cpp` | 0 | 0 | 3 | 0 | 3 |
| `runtime/diagnostics/runtime_observation.hpp` | 6 | 0 | 0 | 0 | 6 |
| `runtime/dispatch/application_job_queue.hpp` | 22 | 0 | 0 | 0 | 22 |
| `runtime/dispatch/coroutine_executor.cpp` | 7 | 0 | 0 | 0 | 7 |
| `runtime/dispatch/host_capacity_runtime.hpp` | 0 | 0 | 0 | 2 | 2 |
| `runtime/dispatch/offload_executor.cpp` | 9 | 0 | 0 | 0 | 9 |
| `runtime/eventing/runtime_wake_timer.hpp` | 4 | 0 | 0 | 0 | 4 |
| `runtime/execution/serial_execution_queue.cpp` | 23 | 0 | 0 | 0 | 23 |
| `runtime/execution/state_lane.cpp` | 5 | 0 | 0 | 0 | 5 |
| `runtime/fanout/fanout_location_runtime.cpp` | 0 | 0 | 10 | 0 | 10 |
| `runtime/fanout/raw_fanout_owner.cpp` | 0 | 16 | 0 | 0 | 16 |
| `runtime/foundation/operation_registry.cpp` | 10 | 0 | 0 | 0 | 10 |
| `runtime/handlers/handler_registry.cpp` | 2 | 0 | 4 | 0 | 6 |
| `runtime/host/app.cpp` | 0 | 17 | 7 | 0 | 24 |
| `runtime/http/http_listener.cpp` | 0 | 5 | 0 | 0 | 5 |
| `runtime/locations/in_memory_store_providers.hpp` | 0 | 0 | 7 | 0 | 7 |
| `runtime/locations/location_lifecycle.hpp` | 0 | 0 | 13 | 0 | 13 |
| `runtime/locations/location_runtime.hpp` | 0 | 1 | 12 | 0 | 13 |
| `runtime/locations/service_descriptor_registry.cpp` | 0 | 0 | 5 | 0 | 5 |
| `runtime/locations/store_location_resolvers.hpp` | 0 | 0 | 8 | 0 | 8 |
| `runtime/mesh/mesh_node_host_service.cpp` | 11 | 0 | 5 | 0 | 16 |
| `runtime/mesh/mesh_node_runtime.cpp` | 14 | 0 | 24 | 0 | 38 |
| `runtime/mesh/raw_mesh_node_owner.cpp` | 0 | 49 | 0 | 0 | 49 |
| `runtime/mesh/route_mesh_runtime_service.cpp` | 15 | 0 | 0 | 0 | 15 |
| `runtime/mesh/service_liveness_registry.cpp` | 0 | 0 | 7 | 0 | 7 |
| `runtime/mesh/service_mailbox.cpp` | 7 | 0 | 0 | 0 | 7 |
| `runtime/mesh/service_topology_registry.cpp` | 0 | 0 | 10 | 0 | 10 |
| `runtime/messaging/logical_multicast_runtime.cpp` | 5 | 0 | 0 | 0 | 5 |
| `runtime/operations/exactly_once_table.hpp` | 10 | 0 | 0 | 0 | 10 |
| `runtime/spots/message_follow_suppression_registry.hpp` | 0 | 0 | 6 | 0 | 6 |
| `runtime/spots/spot_route_internal_dispatcher.cpp` | 1 | 0 | 0 | 0 | 1 |
| `runtime/spots/spot_runtime.cpp` | 31 | 0 | 147 | 0 | 178 |
| `runtime/spots/spot_runtime.hpp` | 0 | 0 | 5 | 0 | 5 |
| `runtime/stateful/maintenance_runtime.cpp` | 9 | 0 | 0 | 4 | 13 |
| `runtime/stateful/public_host_runtime.cpp` | 0 | 0 | 99 | 0 | 99 |
| `runtime/stateful/raw_stateful_dispatch.cpp` | 0 | 3 | 9 | 0 | 12 |
| `runtime/stateful/stateful_object_runtime.cpp` | 0 | 1 | 0 | 0 | 1 |
| `runtime/stateful/stream_session_registry.cpp` | 0 | 2 | 0 | 0 | 2 |
| `runtime/streams/stream_host_service.cpp` | 0 | 68 | 0 | 0 | 68 |
| `runtime/streams/stream_runtime.cpp` | 0 | 12 | 0 | 0 | 12 |
| `runtime/timers/async_delay.hpp` | 2 | 0 | 0 | 0 | 2 |
| `runtime/timers/timer_runtime.cpp` | 6 | 0 | 0 | 0 | 6 |
| `runtime/utils/relocation_id_generator.hpp` | 0 | 0 | 0 | 1 | 1 |
| **합계** | **214** | **230** | **460** | **8** | **912** |

mutex type 문자열은 별도 참고치로 `std::mutex` 194, `std::recursive_mutex` 142,
`std::shared_mutex` 1, 합계 337 token이다. 이는 선언뿐 아니라 parameter/type 언급도 포함하므로
acquisition 계수와 섞지 않았다. `shared_lock` 2곳은 shared mutex read 취득에 포함했다.

## 4. lock 해제·비동기 경계 snapshot 전수 판정

### 4.1 정당화 117곳

아래 표의 source 수 합계가 117이다. 표 안의 `파일:라인` 목록은 해당 분류의 전 목록이며 같은
source는 한 행에만 포함했다.

| 분류 | 수 | source 전 목록 | 근거 |
|---|---:|---|---|
| task completion primitive | 4 | `include/.../contracts/dispatch/task.hpp:252,291,318,342` | completion state와 callback batch를 exact terminal이 소유한다. |
| worker queue | 1 | `include/.../contracts/workers/worker.hpp:309` | queue에서 꺼낸 work item을 현재 worker turn이 소유한다. |
| Actor client submit | 1 | `runtime/actors/actor_client.cpp:153` | `_submitted` terminal과 exact operation이 반환 task를 소유한다. |
| Actor gateway placeholder | 1 | `runtime/actors/actor_gateway_runtime.cpp:1689` | placeholder/token 설치 뒤 같은 token인지 다시 검사한다. |
| raw port I/O | 5 | `runtime/backend/raw_dealer_port.cpp:49,80,112`; `raw_route_port.cpp:78,140` | shared socket operation과 pending result가 exact I/O 수명을 소유하고 close gate가 terminal을 결정한다. |
| channel outbound exact transport/operation | 11 | `runtime/channels/channel_outbound_exchange.cpp:111,119,131,143,407,1098,1223,1343,1425,1492,1555` | callback/boolean은 등록된 shared owner 또는 monotonic 설정이고, 407은 lock 아래 설치한 exact pending request를 이후 await한다. 377/461은 아래 결함 표로 분리했다. |
| channel runtime callback snapshot | 9 | `runtime/channels/channel_runtime.cpp:53,64,75,86,97,108,118,2095,2146` | callback은 shared runtime owner를 보유하며 대상 bundle/lifecycle이 자체 terminal을 적용한다. boolean은 monotonic configuration snapshot이다. |
| client/server location lifecycle | 1 | `runtime/client_server/client_server_location_runtime.cpp:1653` | lifecycle terminal을 선점한 뒤 unbind 작업을 넘긴다. |
| diagnostics observer | 2 | `runtime/diagnostics/logging.cpp:165`; `monitoring_runtime.cpp:287` | subscription snapshot이 shared listener 수명을 소유하고 unsubscribe는 다음 publication만 차단한다. |
| application job queue | 4 | `runtime/dispatch/application_job_queue.hpp:403,591,847,944` | queue head, permit, drain terminal이 다음 실행 순서를 소유한다. |
| serial execution queue | 1 | `runtime/execution/serial_execution_queue.cpp:308` | dequeued item과 running terminal이 exact turn을 소유한다. |
| fanout location | 2 | `runtime/fanout/fanout_location_runtime.cpp:583,813` | lifecycle/worker owner가 현재 publish/remove batch를 terminal까지 소유한다. |
| raw fanout I/O | 1 | `runtime/fanout/raw_fanout_owner.cpp:157` | shared socket operation과 close protocol이 수명을 소유한다. |
| operation registry | 1 | `runtime/foundation/operation_registry.cpp:146` | call ID와 generation으로 exact operation을 완료한다. |
| handler dispatch | 1 | `runtime/handlers/handler_registry.cpp:108` | 선택된 handler와 executor shared lifetime이 callback 실행을 소유한다. |
| host lifecycle | 4 | `runtime/host/app.cpp:614,636,3121,3520` | start/stop 및 listener operation identity가 terminal까지 소유한다. |
| location heartbeat | 1 | `runtime/locations/location_runtime.hpp:112` | heartbeat lifecycle terminal과 cancellation owner가 다음 작업을 막는다. |
| mesh callback/completion | 5 | `runtime/mesh/mesh_node_runtime.cpp:886,1748,1827,2279,4067` | subscription identity, active-count gate, call ID completion이 exact 작업을 소유한다. |
| raw mesh lifecycle/I/O | 8 | `runtime/mesh/raw_mesh_node_owner.cpp:1428,1476,1885,2012,2064,2180,2212,2356` | route/socket shared lifetime, lifecycle gate, exact pending operation이 terminal까지 소유한다. |
| route-mesh pump | 4 | `runtime/mesh/route_mesh_runtime_service.cpp:176,512,521,836` | pump turn과 observer shared lifetime이 batch를 소유한다. |
| Spot route completion | 1 | `runtime/spots/spot_route_internal_dispatcher.cpp:386` | call ID completion state가 exact reply를 소유한다. |
| Spot serial/exact instance | 24 | `runtime/spots/spot_runtime.cpp:526,1818,1990,3415,4312,4637,5507,5547,5597,6259,6319,6914,7312,7488,7958,8680,8696,8707,8802,9212,10251,10276,10462,10493` | serial executor, reservation/generation, callback identity 중 하나가 적용된다. 특히 5507은 lock 안에서 얻은 actor `shared_ptr` 자체가 옛 exact instance를 살리고, 5547은 exact `spot_context_state_t`를 잡아 그 context의 serial owner에서 capture하므로 destroy/recreate 후 현재 map을 갱신하지 않는다. |
| maintenance claim | 1 | `runtime/stateful/maintenance_runtime.cpp:482` | maintenance pass가 claim한 batch와 terminal을 소유한다. |
| public host operation/stage | 16 | `runtime/stateful/public_host_runtime.cpp:1553,1738,2118,2367,2462,2567,2871,3871,3888,4060,4077,4092,4132,4179,5397,6316` | call ID, relocation attempt key, stage terminal 또는 current-entry 재조회가 terminal 반영을 fence한다. |
| stream host I/O | 2 | `runtime/streams/stream_host_service.cpp:2223,3122` | session/socket operation과 close gate가 exact write 수명을 소유한다. |
| stream runtime dispatch | 2 | `runtime/streams/stream_runtime.cpp:247,686` | selected shared session/dispatch owner와 socket terminal이 적용된다. |
| async delay | 1 | `runtime/timers/async_delay.hpp:49` | timer registry entry와 completion task identity가 exact delay를 소유한다. |
| timer runtime | 3 | `runtime/timers/timer_runtime.cpp:154,335,357` | timer generation/cancel identity와 dequeued callback batch가 terminal을 소유한다. |

`spot_runtime.cpp:5507,5547`은 처음에는 “현재 map 재조회 없음” seed였지만 결함으로 세지 않았다.
두 곳 모두 raw pointer만 복사하는 것이 아니라 exact old object를 살리는 `shared_ptr`를 복사하고,
그 옛 객체에만 capture를 수행한다. 경계 뒤 새 generation의 registry를 갱신하지 않으므로 스펙의
exact identity 정당화에 해당한다.

### 4.2 결함 의심 2곳

| 심각도 | 위치 | 짧은 근거 | 의심되는 실패 시나리오 |
|---|---|---|---|
| **[H]** | `runtime/channels/channel_outbound_exchange.cpp:377-413` `channel_native_client_t::request` | `_mutex` 아래 `transport`를 snapshot한 뒤 readiness를 `co_await`하고, L407에서 `transport->mutex`만 잡아 L409의 `transport->socket->request()`를 호출한다. socket null/current 재검증이 없다. | 대기 중 `close()` L529-545가 같은 transport의 `close_noexcept()`를 호출해 L568-584에서 socket을 close/reset할 수 있다. 대기가 false가 아닌 ready로 끝나는 interleaving이면 L409가 null socket을 역참조하거나 이미 종료된 transport에 request를 제출한다. `_closed`와 `socket`을 경계 뒤 같은 gate에서 다시 확인해야 한다. |
| **[H]** | `runtime/channels/channel_outbound_exchange.cpp:461-502` `channel_native_client_t::send` | request와 같은 snapshot 뒤 readiness를 `co_await`하고 L481에서 socket option/send를 사용한다. lock은 reset과 경합을 막을 뿐 이미 reset됐는지는 확인하지 않는다. | close가 먼저 socket을 reset한 뒤 send가 lock을 얻으면 L482의 `transport->socket->options()`가 null을 역참조한다. 또는 endpoint/transport 교체 뒤 stale transport에 제출할 수 있다. exact-current transport와 non-null socket을 경계 뒤 검증해야 한다. |

두 건은 정적 호출·lock ordering으로 가능한 interleaving을 확인한 **결함 의심**이다. race 재현과
수정은 이 읽기 전용 감사 범위가 아니다. `shared_ptr<transport_t>`는 객체 자체의 수명만 연장하며,
그 안의 mutable `unique_ptr<socket>` 현재성을 보장하지 않는다.

## 5. progress T7 보류 2건 현재 상태

| 대상 | 현재 구조와 실측 | 판정 |
|---|---|---|
| `channel_runtime_state_t` | `runtime/channels/channel_runtime.hpp:204-243`의 단일 `mutex`가 channel map, pending, server/client/publisher/subscriber bundle, native client, route/mesh callback, handler, discovery cursor, shutdown을 함께 보호한다. acquisition은 `channel_runtime.cpp` 37곳이며 state lane이 없다. callback을 lock 밖에서 호출하는 것은 필요하지만, 여러 map의 current 관계를 한 C2 region에서 계속 결정한다. | **[M] 미해소, C2 상태 보호.** progress의 “구조 판정 보류”를 닫을 근거가 없고 lane 전환 완료로 셀 수 없다. |
| `mesh_node_runtime_t` | builder 설정은 `runtime/mesh/mesh_node_runtime.hpp:127-151`의 lane이 소유한다. runtime의 mutex는 peer callback active-count gate(L600-606), observed authority map(L630-631), negotiated chunk-limit map(L636-637), message-follow subscription(L658-663), peer intent(L665-668), completion/tombstone table(L670 이후)로 분리돼 있다. acquisition은 38곳 중 실행 primitive 14, 독립 상태 24다. | **조건부 해소.** callback/completion은 protocol gate, 나머지는 서로 다른 keyspace의 C1 registry다. 이들 사이의 양방향 cross-invariant write는 찾지 못했다. 새 의존을 추가할 때는 one-way projection을 유지해야 한다. |

추가로 `spot_node_builder_state_t`는 `runtime/spots/spot_runtime.hpp:71-73`에 builder lane과
`recursive_mutex`를 동시에 두고 `spot_runtime.cpp`에 178 acquisition이 남아 있고,
`public_host_runtime_t`는 route cache lane과 별개로 `public_host_runtime.hpp:1129`의 주 mutex를
99곳에서 취득한다. 따라서 progress의 “잔존=실행 순서 소유 11영역·socket 수명”이라는 서술은
현재 checkout의 460개 상태 보호 acquisition과 양립하지 않는다. 이 progress 수치 drift가
§1의 **[L]** 발견이다.

## 6. blocking bridge 목록과 스펙 06 §5 판정

balanced-parenthesis 정적 scan으로 `lane.run(...)` 호출의 닫는 괄호 직후 `.get()`이 붙는
source를 셌다. 합계는 **456곳**이다. 아래 line은 각 파일의 전 목록이다.

| 파일 | 수 | line 전 목록 | 잔존 사유 |
|---|---:|---|---|
| `runtime/actors/actor_gateway_runtime.hpp` | 1 | 187 | 동기 gateway 설정 표면 |
| `runtime/channels/channel_runtime.cpp` | 1 | 2278 | builder 관측 등록의 return-before |
| `runtime/channels/route_channel_runtime.cpp` | 35 | 26,33,40,47,54,61,71,78,85,93,112,122,130,137,144,151,158,165,176,197,225,250,270,291,324,341,369,389,395,406,413,421,428,481,527 | 동기 route-channel facade |
| `runtime/client_server/client_server_location_runtime.cpp` | 14 | 497,515,538,936,997,1005,1136,1156,1528,1607,1638,1670,1696,1712 | 동기 location/lifecycle 표면 |
| `runtime/client_server/raw_client_server_owner.cpp` | 57 | 186,263,304,312,320,353,405,418,421,432,439,457,502,527,544,569,594,609,654,665,670,677,716,734,780,855,918,959,968,1023,1047,1059,1071,1085,1104,1114,1120,1136,1155,1158,1168,1176,1187,1234,1244,1252,1268,1325,1361,1372,1396,1419,1431,1467,1488,1548,1665 | 동기 transport owner·등록 선행 계약 |
| `runtime/host/app.cpp` | 2 | 1296,1308 | 설정 observer 등록의 return-before |
| `runtime/locations/in_memory_location_store.hpp` | 26 | 49,122,143,180,243,265,296,368,391,436,472,503,522,542,564,748,822,857,973,1091,1166,1211,1354,1485,1523,1543 | public Store task를 반환하기 전 CAS/mutation 완료 |
| `runtime/mesh/mesh_node_runtime.cpp` | 51 | 482,498,532,686,705,720,880,946,1834,4297,4302,4307,4321,4326,4337,4348,4353,4358,4363,4379,4388,4416,4430,4517,4534,4546,4564,4575,4588,4610,4621,4651,4669,4689,4698,4711,4728,4739,4744,4758,4765,4774,4782,4790,4798,4808,4811,4822,4842,4848,4858 | public mesh builder/runtime 동기 표면 |
| `runtime/mesh/raw_mesh_node_owner.cpp` | 47 | 614,680,726,743,769,791,828,944,957,971,1028,1204,1251,1290,1332,1542,1563,1587,1613,1623,1659,1690,1711,1750,1774,2661,2678,2693,2721,2731,2775,2851,2889,2945,2956,3007,3021,3718,3740,3754,3772,3800,3845,3863,3880,3925,3944 | transport lifecycle·route 등록 선행 |
| `runtime/spots/actor_transfer_coordinator.cpp` | 44 | 35,45,60,78,89,108,123,140,161,185,234,285,305,341,357,378,396,411,438,489,513,535,556,587,611,619,628,640,657,669,713,723,736,770,796,816,858,888,916,929,957,972,1000,1007 | 동기 coordinator state-machine API |
| `runtime/spots/spot_runtime.cpp` | 33 | 1788,1800,1820,1857,1874,1902,1936,2185,2200,2211,2223,2231,3780,3799,3837,3896,3924,3939,3953,4026,5649,6336,6704,9138,9154,9330,9940,10291,10321,10618,10640,10672,10687 | Spot public 동기 API와 factory 등록 선행 |
| `runtime/spots/spot_runtime.hpp` | 5 | 457,503,537,621,658 | template public 동기 API |
| `runtime/stateful/maintenance_runtime.cpp` | 19 | 1869,1878,1886,1892,1898,1915,2029,2033,2063,2115,2119,2201,2206,2214,2226,2235,2243,2248,2263 | maintenance lifecycle 동기 owner |
| `runtime/stateful/public_host_runtime.cpp` | 5 | 1344,1354,2383,2411,2433 | route-cache 동기 lookup/update와 등록 선행 |
| `runtime/stateful/raw_stateful_dispatch.cpp` | 23 | 1046,1088,1107,1125,1132,1138,1153,1164,1170,1251,1330,1344,1369,1403,1480,1533,1589,1628,1654,1677,1696,1717,1723 | replay/admission claim의 동기 결정 |
| `runtime/stateful/stateful_object_runtime.cpp` | 56 | 105,120,136,142,221,277,314,353,382,411,467,482,523,554,589,613,627,671,714,735,761,783,855,900,926,951,968,983,1044,1066,1087,1114,1134,1152,1171,1194,1238,1257,1303,1325,1397,1444,1484,1513,1563,1722,1749,1773,1793,1819,1848,1868,1892,1914,1952,2162 | object runtime 동기 facade와 return-before |
| `runtime/stateful/stream_session_registry.cpp` | 31 | 48,84,116,173,265,294,325,364,401,452,472,496,515,547,583,599,650,666,687,712,727,778,797,827,900,949,959,976,986,1020,1033 | session registry 동기 facade |
| `runtime/streams/stream_host_service.cpp` | 6 | 1214,1225,1239,1253,1264,1288 | session handler/route 등록 선행 |
| **합계** | **456** |  |  |

스펙 06 §5의 세 조건 판정은 다음과 같다.

1. **호출자가 가진 lock과 lane 내부 lock이 순환하지 않아야 함 — 충족.** 456곳 중 lexical
   scope상 외부 mutex를 든 채 bridge를 호출하는 18곳을 별도 추적했다
   (`spot_runtime.cpp` 10, `spot_runtime.hpp` 2, `public_host_runtime.cpp` 2,
   `raw_stateful_dispatch.cpp` 4). lane lambda가 그 외부 mutex를 다시 취득하는 경로는 없었다.
   `state_lane_t::throw_if_reentrant()`도 같은 lane 재진입을 거부한다.
2. **completion이 lane ownership을 상속한 inline continuation을 실행하지 않아야 함 — 충족.**
   `runtime/execution/state_lane.hpp:37-65`의 `run`은 `std::promise/std::future`를 만들고 lane
   work가 `set_value/set_exception`만 수행한다. C++ `future::get()`에는 inline continuation
   callback이 없고, 호출 thread는 lane의 thread-local current marker를 상속하지 않는다.
3. **동기 표면을 유지할 실제 계약 사유가 있어야 함 — 충족.** 위 18개 파일 묶음은 public
   synchronous facade, Store의 condition-check/mutation 선행, callback/handler 등록 선행,
   lifecycle/coordinator의 즉시 admission 중 하나다. 비동기로 바꾸면 발견 8의 호환성 또는 발견
   9의 return-before 관찰 순서를 바꾼다.

따라서 456곳 자체를 correctness 결함으로 세지 않는다. 다만 bridge마다 promise/closure 생성과
thread block을 수반하므로 수량은 높고, 새 동기 API를 정당화하는 근거가 될 수 없다. public 계약을
유지하면서 내부 caller를 lane turn 안으로 합칠 수 있는지는 §7의 첫 측정 대상이다.

## 7. POSDDD ② 잔여 후보 상위 10개

POSDDD의 “측정 없이 최적화하지 않는다”는 기준으로 정적 빈도, payload 크기 가능성, 중앙
ownership region 여부를 함께 본 우선순위다. profile/benchmark를 실행하지 않았으므로 아래는
모두 **측정 대상**이며 성능 결함 확정이 아니다.

| 순위 | 위치 | 불필요 할당·복사·경합 후보 | 먼저 측정할 값 |
|---:|---|---|---|
| 1 | `runtime/execution/state_lane.hpp:37-65`, 456개 `.run(...).get()` | bridge마다 `shared_ptr<promise>`, future, work/error closure를 만들고 호출 thread를 block한다. | bridge별 alloc 수, queue wait, blocked thread time, 내부 caller 비율 |
| 2 | `runtime/spots/spot_runtime.hpp:72-73`, `spot_runtime.cpp` 178 acquisition | builder lane 옆의 전역 `recursive_mutex`가 factory, instance, context, relocation 상태를 함께 직렬화한다. | lock hold/wait p95, recursive depth, contention 상위 call site |
| 3 | `runtime/stateful/public_host_runtime.hpp:1129`, `public_host_runtime.cpp` 99 acquisition | completion, relocation attempt, terminal, session/Spot 상태가 큰 주 mutex에 모인다. | hold/wait p95, relocation 중 waiter 수, critical-section copy bytes |
| 4 | `runtime/channels/channel_runtime.hpp:204-243`, `channel_runtime.cpp` 37 acquisition | channel/bundle/callback/discovery cursor가 한 C2 mutex를 공유하며 snapshot map/vector 복사가 뒤따른다. | channel 수별 hold time, callback snapshot bytes, contention |
| 5 | `runtime/streams/stream_host_service.cpp` 68 acquisition, 특히 `:3122-3156` | socket/session close와 제출이 같은 수명 gate에서 직렬화되고 unlock/relock도 집중된다. | socket lock wait, submit당 hold time, close 경합률 |
| 6 | `runtime/mesh/raw_mesh_node_owner.cpp:132-147,1434-1439,2767,3242-3244` | routing/header/payload를 중첩 `vector<uint8_t>`와 multipart vector로 materialize한다. | command별 copied bytes, vector capacity 증가, multipart 크기 |
| 7 | `runtime/mesh/mesh_node_runtime.cpp:4152-4233` | encoded metadata를 `std::vector<uint8_t>(encoded)`로 node/channel/spot send 경계마다 복사한다. | send당 metadata copy bytes와 payload 대비 비율 |
| 8 | `runtime/locations/in_memory_location_store.hpp:144,266,1547-1573,1624` | query/aggregate마다 matched/key/entry vector를 새로 materialize하고 후속 정렬·page 처리를 한다. | row 수별 alloc·copied element 수, page latency |
| 9 | `runtime/spots/spot_runtime.cpp:5529-5533,5581-5584` | relocation capture의 `vector<byte>`를 reserve 후 byte 단위 loop로 `vector<uint8_t>`에 재복사한다. | relocation state 크기별 copied bytes와 capture 시간 |
| 10 | `runtime/mesh/mesh_node_runtime.cpp:1137,1296-1297,1534-1535,3005-3007,3796,3891,4081` | relocation/request/completion마다 `shared_ptr` completion source와 보조 bool/outcome 객체를 개별 할당한다. | operation당 heap alloc 수, outstanding completion high-water mark |

계약상 ownership을 위해 필요한 `shared_ptr`와 방어 복사는 profile 결과 없이 제거하면 안 된다.
특히 #2~#5는 lock 개수가 아니라 실제 wait/hold 분포가 우선이며, #6~#10은 payload 크기와 호출
빈도를 곱한 copied bytes가 우선이다.

## 8. 수행/미수행 범위와 최종 판정

- 수행: production C++ 266파일의 lock token·명시적 재취득 전수 정적 scan, acquisition 912곳의
  파일별/ownership-region 분류, 경계 seed 119곳의 control-flow·호출자 추적, blocking bridge
  456곳과 T7 2건 재판정, POSDDD ② 정적 후보 선정
- 미수행: build, compile, unit/E2E, runtime race 재현, benchmark, CPU/heap/lock profile
- 제외: `core/src`, test/tests, 생성 산출물. core transport는 socket lifetime 경계의 사유 확인에만
  참고했고 계수하지 않음
- 변경: 이 문서 1개만 생성. source와 frozen common spec은 변경하지 않음

최종적으로 C++ CP3는 **NOT CLEAN**이다. 직접 correctness 위험은 `channel_native_client_t`의
두 stale transport snapshot [H]이고, 구조적으로는 T7의 `channel_runtime_state_t` C2 보류가
남아 있다. 117개 정당화 source와 456개 blocking bridge의 조건 충족은 “mutex가 있으므로 안전”한
것이 아니라 exact identity, serial owner, lifecycle terminal, 경계 뒤 재조회 또는 명시적 동기
호환 계약을 source별로 확인한 결과다. 후속 변경에서는 두 [H] 경계에 exact-current/non-null
socket fence를 적용하고 focused close-vs-request/send race test를 추가한 뒤, C2 channel state의
소유 lane 전환 여부를 별도로 결정해야 한다.
