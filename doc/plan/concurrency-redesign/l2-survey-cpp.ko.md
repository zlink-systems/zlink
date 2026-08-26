# cpp 조사 — (codex sol, 2026-08-26)

> 감독: Claude. codex 조사 최종 보고 전문이다.

# C++ L2 전환 순서 조사 (2026-08-26)

## 결론

`framework/languages/cpp/framework/src` 전체를 정적으로 조사한 결과:

- mutex 객체를 직접 소유하는 class/struct: **102개**
- class method의 function-static mutex까지 포함한 소유 단위: **103개**
- class member mutex 선언: **128개**
  - `std::mutex` 126개
  - `std::recursive_mutex` 2개
- RAII lock 취득문: **1,274곳**
- 별도 제외: 함수 자동 지역 mutex 1개와 namespace/function-static mutex 3개. 이들은 클래스 전환 단위가 아니다.
- 취득 지점 15개 이상 또는 `recursive_mutex` 보유 후보: **19개**
- 상위 후보는 모두 **C2**다. `serial_execution_queue_t`도 상태 형태는 C2지만 lane primitive이므로 L2 state-owner 전환 대상에서 제외해야 한다.

계수 기준은 `lock_guard`·`unique_lock`·`scoped_lock` 객체가 생성되는 source 위치다. `scoped_lock`이 mutex 여러 개를 동시에 잡아도 클래스 취득 지점은 1곳으로 셌다. 이미 생성된 `unique_lock.lock()` 재획득은 별도 동작이지 새 lock 선언 지점이 아니므로 기존 조사와 같은 방식으로 제외했다.

현재 branch는 Git 금지 조건 때문에 확인하지 않고 사용자 제공값 `refactor/lane-ownership-concurrency`를 기준으로 했다.

## 전수 목록

표기는 `mutex 선언 수 / lock 취득 지점 수`다. 같은 이름의 local `state_t`는 선언 위치를 함께 표시했다.

| 영역 | mutex 보유 클래스·구조체 |
|---|---|
| actors | `actor_create_call_state_t` 1/4; `actor_client_impl_t` 1/5; `bound_session_delivery_fence_t` 1/3; `session_actor_binding_context_t` 1/7; `actor_gateway_state_t` 1/71(recursive) |
| backend | `raw_dealer_port_t` 1/5; `raw_route_port_t` 1/6 |
| channels | `channel_host_service_t::server_loop_t` 1/3; `transport_t` 1/5; `readiness_state_t` 1/4; `channel_native_client_t` 1/3; `channel_native_publisher_t` 1/2; `channel_runtime_state_t` 1/54; `channel_runtime_bundle_t` 1/6; `route_channel_runtime_t` 2/33 |
| client-server | `client_server_location_runtime_t::pump_task_state_t` 1/2; `client_server_location_runtime_t` 2/19; `raw_client_server_client_t::control_reply_state_t` 1/5; `raw_client_server_server_t` 2/20; `raw_client_server_client_t` 2/26 |
| codecs | `serializer_registry_state_t` 1/2 |
| configuration | `endpoint_connections_state_t` 1/7 |
| diagnostics | `listener_status_registry_t` 1/3; `logging_state_t` 1/12; `monitoring_runtime_state_t` 1/3; `runtime_observer_state_t<T>` 1/6 |
| dispatch | `receive_flow_socket_entry_t` 1/2; `blocking_state_t` 1/2; `application_job_queue.hpp:628 state_t` 1/13; `application_job_queue.hpp:974 state_t` 1/5; `coroutine_executor_t` 1/2; `host_capacity_runtime_t` 1/2; `offload_executor_t` 1/9 |
| eventing | `runtime_wake_timer_t` 1/4 |
| execution | `serial_deferred_barrier_t` 1/4; `serial_turn_handle_impl_t` 1/4; `serial_execution_queue_t` 1/15; `state_lane_t` 1/5 |
| fanout | `fanout_location_runtime_t` 1/10; `raw_fanout_publisher_t` 1/7; `raw_fanout_subscriber_t` 1/9 |
| foundation | `operation_registry.cpp:123 state_t` 1/5; `operation_registry_t` 1/5 |
| handlers | `filter_next_state_t` 1/4 |
| host | `relocation_operation_t` 1/8; `termination_operation_t` 1/3; `app_state_t` 1/4; `framework_runtime_status_source_t` 2/5 |
| http | `http_host_service_t::listener_t` 2/5 |
| locations | `in_memory_location_repository_t` 1/26; `in_memory_location_store_t` 1/3; `in_memory_relocation_store_t` 1/4; `location_lifecycle.hpp:201 state_t` 1/13; `location_runtime_t` 2/13; `service_descriptor_registry_t` 1/5; `actor_location_observer_t` 1/1; `store_location_resolvers_t` 1/7 |
| mesh | `mesh_node_host_service_t::actor_destroy_callback_gate_t` 1/3; `mesh_node_host_service_t` 2/11; `mesh_node_runtime_t::message_follow_subscription_state_t` 1/3; `mesh_node_builder_state_t` 1/47; `peer_callback_gate_t` 1/7; `mesh_node_runtime_t` 5/21; `raw_mesh_node_owner_t` 2/48; `hub_t` 1/14; `route_mesh_runtime_service_t::state_t` 2/1; `service_liveness_registry_t` 1/7; `service_mailbox_t` 1/7; `service_topology_registry_t` 1/10 |
| messaging | `logical_multicast_executor_t` 1/5 |
| operations | `exactly_once_table_t` 1/10 |
| spots | `actor_transfer_coordinator_t` 1/44; `message_follow_suppression_registry_t` 1/6; `spot_route_internal_dispatcher.cpp:374 completion_state_t` 1/1; `remote_actor_commit_turn_state_t` 1/12; `remote_actor_commit_deadline_t` 1/3; `actor_handoff_barrier_t` 1/3; `spot_runtime.cpp:1954 async_state_t` 1/3; `spot_runtime.cpp:7883 completion_state_t` 1/1; `submission_state_t` 1/5; `deadline_state_t` 1/3; `spot_node_builder_state_t` 2/147(recursive 1); `spot_context_state_t` 1/26 |
| stateful | `maintenance_runtime_t` 2/5; `shutdown_tracking_state_t` 1/2; `host_maintenance_runtime_t` 1/19; `public_host_runtime_t` 2/110; `raw_stateful_dispatch_t` 1/9; `raw_relocation_replay_coordinator_t` 1/19; `stateful_object_runtime_t` 1/50; `stream_session_registry_t` 1/2 |
| streams | `stream_receive_scheduler_t` 1/10; `stream_async_operation_state_t` 1/2; `session_liveness_t` 1/6; `replacement_session_state_t` 1/7; `core_session_t` 1/4; `stream_write_wait_state_t` 1/3; `stream_write_queue_t` 1/3; `stream_host_service_t::listener_t` 8/36; `stream_state_t` 3/9 |
| timers | `async_delay_timer_t` function-static 1/2; `timer_state_t` 1/8 |
| utils | `relocation_id_generator_t` 1/1 |

## 상위 후보

여러 컬렉션 수는 lock 수명 범위 안에서 둘 이상의 소유 컬렉션을 직접 명명한 보수적 계수다. helper 내부 접근까지 의미적으로 펼치면 수치가 커질 수 있다.

### `spot_node_builder_state_t`

[spot_runtime.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.hpp:57)

- lock **147곳**: recursive `mutex` 143, `actor_pending_requests_mutex` 4.
- 보호 상태: Spot factory/lifecycle/context/pending-creation, Actor 위치·generation·authority fence·instance·queue·destroy 상태, handoff 요청·remote cleanup, native Spot/node, idle eviction과 relocation readiness.
- 여러 컬렉션 직접 접근: **25/147**. 예: actor generation·created/destroyed set을 함께 쓰는 [spot_runtime.cpp:1302](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:1302), Spot ID/context와 Actor 위치를 함께 쓰는 [spot_runtime.cpp:2554](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:2554).
- 판정: **C2**.
- 파급: 구체 타입 **4파일/75참조**, lock-bearing 문맥 중 coroutine/async 약 **31%**.
- 재진입 실측: `record_actor_spot()`이 mutex를 잡은 채 [spot_name_for()를 호출](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:9889)하고, 후자가 [같은 mutex를 다시 획득](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:9755)한다. 이것은 `fast_mutex` 치환 시 즉시 실패할 확정 지점이다.
- lane 밖 수명: `spot_context_state_t`, native Spot, routed control Spot, relocation-ready state를 `shared_ptr` vector로 꺼내 lock 밖 callback/close에서 사용한다. 예: native Spot snapshot [L10120](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:10120). 스냅샷이 owner보다 오래 유지될 수 있으므로 generation/closed fence를 보존해야 한다.
- 장기 작업: idle-eviction timer, worker executor의 handoff replay/cleanup, actor-join completion, relocation replay, detached timer-destruction thread 등 최소 **5개 논리 시작점**.
- POSDDD: 단일 recursive mutex 경합, snapshot vector 및 packet 복사, per-operation `shared_ptr`가 크다. 상태 aggregate 자체는 응집되어 있으므로 분할보다 lane 전환과 재진입 해체가 먼저다.

### `public_host_runtime_t`

[public_host_runtime.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.hpp:602)

- lock **110곳**: `_mutex` 105, route-cache mutex 5.
- 보호 상태: completion, local Spot request/deadline/dispatch, Spot·Actor index, peer endpoint, session seal/journal, relocation assembly/attempt, user-Spot terminal과 lifecycle flag.
- 여러 컬렉션 직접 접근: **5/110**. 대표적으로 종료 정리 [public_host_runtime.cpp:1126](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:1126)와 request/deadline terminal [L6274](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:6274).
- 판정: **C2**.
- 파급: **7파일/147참조**, async 약 **25%**.
- 재진입: 동일 non-recursive mutex 중첩은 확인되지 않았다. 구현은 이미 재획득을 피하려고 lock 없이 읽는 구간을 명시한다([public_host_runtime.cpp:1242](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:1242)).
- lane 밖 수명: relocation attempt·completion·callback·transport를 값 또는 `shared_ptr`로 반출한 뒤 비동기 전송한다. 이미 지워진 attempt와 늦은 completion을 구분하는 exact key/fence를 유지해야 한다.
- 장기 작업: relocation target polling, session-route submission, local request expiry, remote operation completion 등 약 **7개 시작점**([L2758](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:2758), [L2935](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:2935)).
- POSDDD: 가장 큰 central gate다. relocation/session/application 자료구조가 한 lock에 결합되고 wire payload·terminal snapshot 복사가 반복된다. 죽은 코드는 확인되지 않았다.

### `actor_gateway_state_t`

재조사하지 않고 [기존 L1 조사](/home/hep7/project/zlink/doc/plan/concurrency-redesign/l1-survey-cpp.ko.md:40)를 따른다.

- recursive mutex 1개, lock **71곳**, 여러 컬렉션 **21/71**, **C2**.
- 동일 mutex의 실제 중첩 재획득은 기존 조사에서 0곳이었다.
- sink·binding snapshot·handler vector가 lock 밖으로 나가며, detached send drain과 binder compensation이 있다.
- `fast_mutex` 치환은 진단용 후보지만, abort가 발생할 때의 stack을 실제 재진입 근거로 삼아야 한다.

### `channel_runtime_state_t`

[channel_runtime.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/channels/channel_runtime.hpp:138)

- lock **54곳**, mutex 1개.
- 보호 상태: channel 설정, pending request/counter, server/client/publisher/subscriber bundle, native client/publisher, route channel, mesh/channel/Spot sender·requester callback.
- 여러 컬렉션 직접 접근: 보수적으로 **2/54**. sender/requester pair를 함께 갱신하는 [channel_runtime.cpp:682](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:682), [L693](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:693).
- 판정: **C2**. callback registry와 pending state를 읽은 뒤 async send/request를 수행한다.
- 파급: **7파일/98참조**, async 문맥 약 **1/3**.
- 재진입: 중첩 획득은 확인되지 않았다.
- lane 밖 수명: route channel `shared_ptr`를 snapshot한 뒤 stop하는 [channel_runtime.cpp:568](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:568) 형태가 대표적이다. 제거 후에도 이미 반출된 transport가 한 번 더 호출될 수 있다.
- 장기 작업: route offload executor와 async request/reply chain.
- POSDDD: 다수의 type-erased callback map과 `shared_ptr` bundle 복사, 단일 gate 경합이 측정 후보다.

### `stateful_object_runtime_t`

[stateful_object_runtime.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.hpp:260)

- lock **50곳**, mutex 1개.
- 보호 상태: placement 후보, object/generation/attempt, membership move, Spot close, relocation seal/hold/restore reservation과 queue capacity.
- 여러 컬렉션 직접 접근: **19/50**.
- 판정: **C2**.
- 파급: **8파일/85참조**, coroutine 비중은 낮지만 aggregate seal 1개가 핵심 async 경계다.
- 재진입: unlocked helper를 사용하며 중첩 획득은 확인되지 않았다.
- lane 밖 수명 및 §8-3 단서: [stateful_object_runtime.cpp:1023](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:1023)은 `shared_ptr<unique_lock<mutex>>`를 coroutine frame에 넣고, [L1080–1090](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:1080)에서 unlock→`co_await`→relock한다. mutex owner 수명보다 lock 객체가 길어질 수 있는 가장 직접적인 `fast_mutex EINVAL-on-unlock` 조사 단서다.
- 장기 작업: aggregate pre-capture fence await 1곳. 자체 timer/loop는 없다.
- POSDDD: `shared_ptr<unique_lock>`와 object/frozen vector materialization은 복잡성과 수명 위험을 함께 만든다. L2 초반에 별도 집중 전환할 가치가 크다.

### `raw_mesh_node_owner_t`

[raw_mesh_node_owner.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.hpp:166)

- lock **48곳**: lifecycle 43, socket 5.
- 보호 상태: port/poller/socket·monitor, lifecycle generation, expected/connected peer, operation/reply route와 close 상태.
- 여러 컬렉션 직접 접근: **0/48** 직접 명명. 하지만 port snapshot과 lifecycle scalar/fence가 하나의 불변식을 구성한다.
- 판정: **C2**.
- 파급: **8파일/105참조**, async 약 **44%**.
- 재진입: 중첩 획득은 확인되지 않았다.
- lane 밖 수명: `_port`를 `shared_ptr`로 복사한 뒤 lock 밖에서 `co_await`한다([raw_mesh_node_owner.cpp:1204](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1204), [L1242](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1242)). close 이후에도 snapshot port가 존재하므로 terminated/generation fence가 필요하다.
- 장기 작업: request retry/completion, monitor drain, liveness tick.
- POSDDD: header/payload vector 복사와 central lifecycle mutex가 주요 비용이다.

### `mesh_node_builder_state_t`

[mesh_node_runtime.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.hpp:121)

- lock **47곳**, mutex 1개.
- 보호 상태: endpoint/routing/role/capacity 설정, channel·handler·peer 등록, service registrar, context/job queue와 Spot builder projection.
- 여러 컬렉션 직접 접근: **1/47**.
- 판정: **C2**. 여러 설정 field와 collection이 sealed builder snapshot을 함께 결정한다.
- 파급: **7파일/27참조**, async 약 **0%**.
- 재진입: 중첩 획득은 확인되지 않았다.
- lane 밖 수명: context, application queue, handler-group, Spot state를 `shared_ptr`로 반출해 runtime 구성에 사용한다. builder sealing 이후 변경 차단이 수명 fence다.
- 장기 작업: 자체 시작점 0.
- POSDDD: 비교적 낮은 위험의 동기 구성 상태다. snapshot/vector 복사는 있지만 hot path가 아니다.

### `actor_transfer_coordinator_t`

[actor_transfer_coordinator.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/actor_transfer_coordinator.hpp:170)

- lock **44곳**, mutex 1개.
- 보호 상태: move/admission/completed-admission/backlog/message-follow route와 transfer ID.
- 여러 컬렉션 직접 접근: **15/44**.
- 판정: **C2**.
- 파급: **3파일/46참조**, async **0%**. 호출자는 비동기지만 coordinator 표면은 동기 상태 전이다.
- 재진입: 중첩 획득은 확인되지 않았다.
- lane 밖 수명: backlog·admission·message-follow target을 값으로 꺼낸다. `shared_ptr` mutable capability는 없지만 vector/message 복사본은 이후 stale할 수 있어 transfer/fence identity가 필요하다.
- 장기 작업: 자체 timer/task 0. 호출자가 expiry/reconciliation을 구동한다.
- POSDDD: packet vector 이동·복사와 하나의 gate에 move/follow/terminal 상태를 함께 태우는 경합이 관찰점이다.

### `stream_host_service_t::listener_t`

[stream_host_service.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/streams/stream_host_service.cpp:953)

- mutex **8개**, 고유 lock 취득 **36곳**.
- 보호 상태: active/retired session, io/socket/worker, core socket, pending disconnect, core session/frame assembler, readiness.
- 여러 컬렉션 직접 접근: **3/36**. `_active_streams`와 core session map을 함께 잡는 [L1230](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/streams/stream_host_service.cpp:1230)이 대표적이다.
- 판정: **C2**.
- 파급: 구현 파일 1개에 갇히지만 listener 전체가 약 3천 줄이며 async/network 비중이 높다.
- 재진입: 동일 mutex 중첩은 확인되지 않았다. 대신 여러 mutex의 순서와 scoped two-lock 획득이 있다.
- lane 밖 수명: `shared_ptr<core_session_t>`와 socket/session snapshot이 async read/write 및 retire 경계를 넘는다.
- 장기 작업: listener worker thread, asio loop, accept retry, liveness sweep, detached wake timer([L4083](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/streams/stream_host_service.cpp:4083)).
- POSDDD: 8개 mutex의 순서 추론 비용과 frame/session map 복사가 크다. 단일 lane으로 옮길 상태와 실제 socket/io mutex를 먼저 구분해야 한다.

### `route_channel_runtime_t`

[route_channel_runtime.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/channels/route_channel_runtime.hpp:29)

- lock **33곳**: state mutex 31, backend mutex 2.
- 보호 상태: connection/ready peer, pending request, outbound packet, manual endpoint, routing/running 설정과 backend callback.
- 여러 컬렉션 직접 접근: **3/33**.
- 판정: **C2**.
- 파급: **8파일/47참조**, async 약 **0%**.
- 재진입: unlocked helper를 사용하며 중첩 획득은 확인되지 않았다.
- lane 밖 수명: backend callback을 복사해 lock 밖에서 호출한다. `shared_ptr` snapshot은 없고 packet/value snapshot이 주로 남는다.
- 장기 작업: 자체 background task 0; blocking condition wait가 있다.
- POSDDD: outbound frame vector 복사와 동기 wait가 관찰 대상이다.

### `in_memory_location_repository_t`

[in_memory_location_store.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/locations/in_memory_location_store.hpp:30)

- lock **26곳**, mutex 1개.
- 보호 상태: lease, mesh/client-server/fanout row, Entry Spot claim/stamp, authority, reservation, terminal, aggregate/capacity.
- 여러 컬렉션 직접 접근: **6/26**.
- 판정: **C2**.
- 파급: **2파일/4참조**, async **0%**.
- 재진입·장기 작업: 0/0.
- lane 밖 수명: 대부분 값 반환이며 mutable `shared_ptr` snapshot은 확인되지 않았다.
- POSDDD: list/snapshot 정렬과 전역 gate 경합이 있지만 in-memory provider이므로 전환 난도와 운영 우선순위는 낮다.

### `spot_context_state_t`

[spot_runtime.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.hpp:433)

- lock **26곳**, callback mutex 1개.
- 보호 상태: callback admission/owner thread/depth, closed/idle-eviction, relocation boundary/readiness.
- 여러 컬렉션 직접 접근: **0/26**.
- 판정: **C2**. scalar들이 callback admission 불변식을 이루고 async handler 전후로 바뀐다.
- 파급: **4파일/78참조**, async 약 **8%**.
- 재진입: callback depth로 논리적 재진입을 허용·추적하지만 mutex 자체 중첩은 확인되지 않았다.
- lane 밖 수명: context state가 `shared_ptr`로 handler/timer/relocation completion에 전달된다.
- 장기 작업: serialized handler task, timer callback, relocation-ready completion.
- POSDDD: type-erased callback map과 `shared_ptr` timer/handler instance가 많다. callback admission과 timer collection을 같은 전환에서 임의로 합치면 안 된다.

### `raw_client_server_client_t`

[raw_client_server_owner.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/client_server/raw_client_server_owner.hpp:152)

- lock **26곳**: state 21, socket 5.
- 보호 상태: connection/admission/readiness, liveness, pending response, monitor/socket lifecycle.
- 여러 컬렉션 직접 접근: **0/26** 직접 명명.
- 판정: **C2**.
- 파급: **4파일/41참조**, async 비중이 높다.
- 재진입: 중첩 획득은 확인되지 않았다.
- lane 밖 수명: port/socket 및 completion state를 `shared_ptr`로 반출해 pump/request 경계를 넘는다.
- 장기 작업: pump, admission/control, liveness.
- POSDDD: request payload 복사와 state/socket 이중 gate 순서가 관찰점이다.

### `mesh_node_runtime_t`

[mesh_node_runtime.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.hpp:177)

- mutex **5개**, lock **21곳**.
- 보호 상태: observed Spot authority, receive chunk limit, message-follow subscription, peer callback, request completion.
- 여러 컬렉션 직접 접근: **0/21**. 각 mutex는 대체로 별도 collection을 지킨다.
- 판정: **C2**. completion·peer callback을 반출한 뒤 async network 행동이 이어진다.
- 파급: **15파일/197참조**로 타입 노출이 가장 넓다.
- 재진입: 중첩 획득은 확인되지 않았다.
- lane 밖 수명: peer callback/completion/subscription이 `shared_ptr` 또는 callback snapshot으로 반출된다.
- 장기 작업: message-follow subscription, request completion, peer callback.
- POSDDD: lock 수보다 넓은 facade 파급과 callback lifetime이 난도를 결정한다.

### `raw_client_server_server_t`

[raw_client_server_owner.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/client_server/raw_client_server_owner.hpp:70)

- lock **20곳**: state 15, socket 5.
- 보호 상태: peer admission/monitor event/pending receive/liveness와 router socket lifecycle.
- 여러 컬렉션 직접 접근: **0/20**.
- 판정: **C2**.
- 파급: **4파일/27참조**, async pump/liveness 비중이 높다.
- 재진입: 중첩 획득은 확인되지 않았다.
- lane 밖 수명: received record·application permit·route port를 반출해 async dispatch한다.
- 장기 작업: pump와 liveness tick.
- POSDDD: client와 같은 socket/state 이중 gate 및 frame 소유권 복사가 관찰점이다.

### `client_server_location_runtime_t`

[client_server_location_runtime.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/client_server/client_server_location_runtime.hpp:38)

- lock **19곳**: state 14, descriptor-publish 5.
- 보호 상태: server/client map, snapshot sequence/last snapshot/observer, pump cursor/snapshot, ready waiter와 descriptor publication.
- 여러 컬렉션 직접 접근: **5/19**.
- 판정: **C2**.
- 파급: **4파일/57참조**, async 약 **5%**지만 전용 worker thread가 상태를 지속적으로 변경한다.
- 재진입: 중첩 획득은 확인되지 않았다.
- lane 밖 수명: server/client owner와 pump task를 `shared_ptr` snapshot으로 보존한다.
- 장기 작업: runtime thread [client_server_location_runtime.cpp:613](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/client_server/client_server_location_runtime.cpp:613), wake timer, server/client pump.
- POSDDD: 반복 snapshot materialization과 polling thread가 관찰점이다.

### `host_maintenance_runtime_t`

[maintenance_runtime.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/maintenance_runtime.hpp:687)

- lock **19곳**, mutex 1개.
- 보호 상태: admission state, active/inventory/shutdown flag, effective intent, attempt/result map, completion과 terminal.
- 여러 컬렉션 직접 접근: **0/19**.
- 판정: **C2**. scalar·completion·attempt result의 lifecycle 불변식 때문이다.
- 파급: **3파일/21참조**, async 약 절반.
- 재진입: 중첩 획득은 확인되지 않았다.
- lane 밖 수명: active completion `shared_ptr`와 observer를 반출한 뒤 완료한다.
- 장기 작업: retire/termination attempt coroutine.
- POSDDD: attempt마다 completion allocation과 inventory snapshot 복사가 관찰점이다.

### `raw_relocation_replay_coordinator_t`

[raw_stateful_dispatch.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/raw_stateful_dispatch.hpp:221)

- lock **19곳**, mutex 1개.
- 보호 상태: target/source-terminal/target-terminal map, active-stage·closing·acknowledging 상태, retained byte count.
- 여러 컬렉션 직접 접근: **0/19** 직접 명명.
- 판정: **C2**.
- 파급: **6파일/43참조**, async 약 **26%**.
- 재진입: target activity guard가 unlock 후 owner로 돌아오지만 중첩 획득은 확인되지 않았다.
- lane 밖 수명: registration/callback/payload를 값으로 snapshot하여 transport await 후 exact key로 재검증한다.
- 장기 작업: terminal relay retry와 mailbox pump.
- POSDDD: retained payload 복사와 target/terminal을 같은 gate에 태우는 경합이 관찰점이다.

### `serial_execution_queue_t`

[serial_execution_queue.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/execution/serial_execution_queue.hpp:155)

- lock **15곳**, mutex 1개.
- 보호 상태: application/lifecycle lane, deferred work, active turn/lane/bytes, claim, close/drain 상태.
- 여러 컬렉션 직접 접근: 보수적으로 **6/15**.
- 판정은 **C2**지만 **L2 전환 대상 제외**. 이것은 state-owner 후보가 아니라 application/lifecycle lane primitive다.
- 파급: **9파일/117참조**.
- 재진입과 장기 drain은 primitive 계약 자체의 일부다.
- lane 밖 수명: active turn과 deferred callback이 `shared_ptr`/function으로 executor에 전달된다.
- POSDDD: admission hot path 경합은 별도 primitive 성능 작업에서 측정해야 한다.

## §8-3 `fast_mutex` 단서

우선순위는 다음 두 곳이다.

1. **확정 중첩 재획득**: `spot_node_builder_state_t`의 `record_actor_spot()` → `spot_name_for()`([9889](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:9889) → [9755](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:9755)). Non-recursive 진단 mutex로 바꾸면 즉시 실패해야 한다.
2. **mutex owner와 guard 수명 분리**: `stateful_object_runtime_t`의 `shared_ptr<unique_lock<mutex>>`가 coroutine suspension을 넘는다([1023](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:1023) → [1080](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:1080)). `fast_mutex.hpp:76`의 EINVAL-on-unlock가 “소유 객체가 먼저 파괴됨”이라면 가장 직접적으로 연결되는 형태다.

`actor_gateway_state_t`는 recursive mutex지만 기존 조사에서 실제 중첩 획득이 없었다. 따라서 진단 우선순위는 위 두 곳보다 낮다.

## 제안 전환 순서

배수 기준은 전환 완료된 C++ L1 표본 `stream_session_registry_t`다.

1. `mesh_node_builder_state_t` — **0.7배**. 동기 구성 상태이고 장기 작업이 없다.
2. `in_memory_location_repository_t` — **0.8배**. 호출 파급이 거의 없고 async 경계가 없다.
3. `host_maintenance_runtime_t` — **0.9배**. 상태는 작지만 completion 수명을 정리해야 한다.
4. `raw_relocation_replay_coordinator_t` — **1.0배**. exact key 재검증 구조가 이미 있다.
5. `actor_transfer_coordinator_t` — **1.1배**. 여러 컬렉션 전이가 많지만 표면이 동기다.
6. `route_channel_runtime_t` — **1.1배**. backend callback 반출과 condition wait를 분리해야 한다.
7. `spot_context_state_t` — **1.2배**. callback depth와 async 실행 문맥 상속을 정리해야 한다.
8. `client_server_location_runtime_t` — **1.3배**.
9. `raw_client_server_server_t` — **1.3배**.
10. `raw_client_server_client_t` — **1.5배**.
11. `channel_runtime_state_t` — **1.5배**. callback registry와 route transport 수명이 넓다.
12. `mesh_node_runtime_t` — **1.7배**. lock은 적지만 구체 타입 참조가 15파일로 넓다.
13. `stateful_object_runtime_t` — **1.8배**, 단 §8-3 조사 때문에 앞당겨도 된다. `shared_ptr<unique_lock>` 제거가 핵심이다.
14. `raw_mesh_node_owner_t` — **2.0배**. lifecycle/socket 및 높은 coroutine 비중.
15. `actor_gateway_state_t` — **기존 조사 기준 약 2배**. recursive mutex는 있으나 중첩 실측 0.
16. `stream_host_service_t::listener_t` — **2.5배**. 8개 mutex와 실제 socket/io 상태를 분류해야 한다.
17. `public_host_runtime_t` — **3.5배 이상**. 110 lock과 relocation/session/application 상태가 결합된다.
18. `spot_node_builder_state_t` — **5배 이상, 최종 배치 권장**. 147 lock, 확정 중첩 재획득, 넓은 Actor/Spot lifecycle과 다수 장기 작업이 있다.

`serial_execution_queue_t`는 순서에서 제외한다. `stateful_object_runtime_t`는 순수 난도 순서보다 앞당겨 작은 집중 배치로 처리하면 `fast_mutex` abort 가설을 L2 초반에 판정할 수 있다.

조사 시간은 **23분 35초**(2026-08-26 16:20:50–16:44:25 KST)였다. 변경 파일은 없으며, Git·빌드·테스트는 실행하지 않았다. 정적 구문 분석과 source 검색만 수행했다.
