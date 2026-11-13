# C++ L5~L7 (#4355~4357) 호출 대조

이전 L4 `276b84f248`, 실험 branch `exp/3868-cpp-ladder-next`. 측정 없음.
`ZLINK_CPP_LADDER_LEVEL=7`은 L5~L7 묶음이다. 같은 build에서 `0/1/2/3/4/7/8`을 고르며 `8`은 기존 main 경로다.
`B=framework/bench/grpc/cpp`, `R=framework/languages/cpp/framework/src/runtime`,
`I=framework/languages/cpp/framework/include/zlink/framework`.
이전 line은 L4 commit, 새 line은 L5_7 구현 commit 기준이다.

| 경로 | 이전 L4 file:line | 새 L5_7 bench file:line | 실제 production 경로·차이의 소유 기능 |
|---|---|---|---|
| source payload 생성·stamp | `B/ladder/ladder_wire.hpp:132-136`, `B/ladder/ladder_payload.hpp:21-26` | `B/client/bench_cpp_client.cpp:713-726` → 같은 `make_typed_payload` | L4의 같은 DTO·기본 JSON·base64 body를 유지한다. 양쪽 모두 payload 내부 stamp로 검증·latency를 처리하며 추가 stamp·protobuf 변환은 없다 |
| source send/request 제출 | `B/ladder/ladder_driver.inc:121-147`: binding socket에 직접 제출 | `B/client/bench_cpp_client.cpp:728-744`: `route_client_t::send_to_node/request_to_node(...).async()` | `I/contracts/channels/channel.hpp:824-843,888-916` → `R/channels/channel_runtime.cpp:1481-1532,1898-1940` → `R/mesh/mesh_node_runtime.cpp:4220-4268,4383-4440` → `R/mesh/raw_mesh_node_owner.cpp:1678-1724,1779-1839`. L5 source owner turn·target 선택·submit, L7 request identity/registry/deadline·pending send completion. 즉시 admission send는 production대로 registry를 만들지 않는다 |
| target receive·owner queue·worker | `R/mesh/raw_mesh_node_owner.cpp:526-541`, `B/ladder/ladder_wire.hpp:403-416`: blocking recv(NONE)→고정 handler. L1 permit 유지 | `B/framework/bench_framework_cpp_server.cpp:91-100`: public RouteMesh 및 typed handler 등록 | `R/mesh/mesh_node_host_service.cpp:2142-2220,2299-2311,3365-3371` → `R/mesh/service_mailbox.cpp:205,588,635` → `R/dispatch/offload_executor.cpp:99,427-508` → `R/mesh/mesh_node_host_service.cpp:3869-3928` → `R/stateful/public_host_runtime.cpp:8523`. L5가 production readiness/receive rotation·drain·mailbox·worker 전환을 복원한다. L4까지의 binding 수신 loop와 다른 부분은 모두 이 복원 범위다 |
| target envelope·typed decode·activation | `B/ladder/ladder_wire.hpp:253-269`: retained frame·header parse·unhosted 기본 serializer·metrics | `B/framework/bench_framework_cpp_server.cpp:23-40,97-100`에 등록한 handler를 runtime이 생성·호출 | `R/mesh/mesh_node_host_service.cpp:3809-3844` → `R/mesh/mesh_record_dispatcher.cpp:47-78` → `R/channels/route_packet_dispatcher.cpp:385-423` → `R/channels/route_handler_invoker.cpp:37-48` → `R/channels/route_handler_registry.hpp:144-149`. L6 dispatch마다 scope와 새 handler를 만들고 DI로 metrics를 받는다. `I/contracts/configuration/services.hpp:321-325`, `I/contracts/codecs/serializer.hpp:496-515`의 production instance/immutable codec plan을 사용한다. L4 JSON decode 의미는 유지하며 codec plan binding은 handler construction plan의 일부다 |
| target reply 새 body·encode | `B/ladder/ladder_wire.hpp:278-296`: 새 DTO에 body 복사→typed JSON encode→reply writer | `B/framework/bench_framework_cpp_server.cpp:26-29`: 같은 metrics 기록 후 request를 값으로 반환해 새 vector body 생성 | `R/channels/route_handler_registry.hpp:147` → `I/contracts/detail/handler_invocation.hpp:97-110` → `R/channels/route_handler_invoker.cpp:70-108`. L6 handler invocation 및 scope 수명, L7 완료 turn. 새 body copy·JSON encode는 양쪽에 각 1회이며 request wire parts를 재사용하지 않는다 |
| target reply framing·submit | `B/ladder/ladder_wire.hpp:302-315`: 직접 binding reply token·2-part submit | 위 public request handler의 반환값을 runtime이 완료 | `R/channels/route_packet_dispatcher.cpp:426-453` → `R/mesh/mesh_record_dispatcher.cpp:64-72` → `R/mesh/raw_mesh_node_owner.cpp:2177-2192`. L7의 보존된 reply route·완료 처리 경로를 복원한다. 기존 L2 response envelope와 L3 service reply framing, 새 2-part submit은 유지한다 |
| source completion·deadline·typed decode | `B/ladder/ladder_driver.inc:40,132-147`, `B/ladder/ladder_wire.hpp:324-348`: binding completion에서 service/header/JSON decode | `B/client/bench_cpp_client.cpp:734-744`: public request task와 admission task 관찰→기존 `finish_bytes/admit` | `R/foundation/operation_registry.cpp:382-466,488,735-772`의 등록·deadline index·만료 → `:270-295`의 terminal dispatch → `R/dispatch/provider_task_context.cpp:172-191` → `R/mesh/mesh_node_runtime.cpp:4383-4410` → `R/channels/channel_runtime.cpp:1822` → `I/contracts/channels/call.hpp:246`. L7 operation registry·terminal 원자적 확정·deadline·completion dispatcher를 복원한다. L3 multipart views는 operation request storage의 PMR resource를 사용하며, L4 기본 typed JSON decode·body stamp 검증은 유지한다 |

L5_7은 기존 production runtime 전체를 public route로 호출하며 Framework 내부 진입점을 새로 만들지 않는다. L4의 직접 socket/codec 경로를 L5_7에 복제하거나 bench queue·worker·registry·timer를 추가하지 않았다. Production runtime 소스는 변경하지 않는다. 기존 driver/handler를 template으로 재사용해 main(8)의 protobuf 동작도 유지한다. 기본 JSON payload·새 reply body·동일 bench metrics·2-part wire contract를 유지한다. Control-plane 시작과 peer admission은 public host lifecycle에 속한다.

| 결정 | 소유 spec | 사용한 소유자 |
|---|---|---|
| source owner turn·receive rotation·mailbox/worker | `01-execution/08-messaging-hot-path.ko.md` §3 E2~E4, §4.1·§4.3·§4.4 | 기존 mesh owner lane·host receive role·mailbox·offload executor |
| dispatch별 handler instance·DI scope | `00-foundation/06-framework-api.ko.md` §10 | `route_handler_invoker`의 invocation scope와 `create_dispatch_instance` |
| operation 등록·deadline·terminal·completion turn | `01-execution/01-submit-and-completion.ko.md` §10·§10.1·§11, `08-messaging-hot-path.ko.md` §3 E3~E5·§4.4 | 기존 operation registry와 completion dispatcher |

Production completion의 실행 자원 전환은 기존 dispatcher 한 곳이 결정한다. 이 작업은 전환 수를 계측하지 않았다. `operation_registry.cpp:281` → `provider_task_context.cpp:172` 외의 bench 재게시 경로는 없다. 이전 L4의 임의 service correlation(`seq+1`)은 L7 operation/ReplyRouteId 소유자가 생성하는 identity로 대체한다. 형식·payload 의미는 바꾸지 않는다.

교차언어 참고: Java `4bf0f1c4bf`의 public route+per-dispatch activation+registry/deadline/completion, .NET `de0507ae48`의 public managed mesh route 묶음, Node `3c50a02908`의 public Framework client/server 묶음과 같은 복원 범위다. Java·Node와 같이 C++ L5_7은 L4의 기본 JSON을 유지한다. .NET 참고 commit의 typed protobuf 표현 차이는 [L4 호출 대조](call-shape-L4.md)에 기록했다. 참고 branch/worktree는 변경하지 않았다.

대안: bench에서 mailbox·worker·scope·operation을 각각 조립하면 production의 소유 결정을 복제한다. 기존 public route를 호출하면 모든 결정을 원래 소유자에 둘 수 있으므로 후자를 구현했다. 분류 A(기존 계약 경로의 실험 단계 연결). 판정 위치 수: queue/worker·activation/scope·registry/deadline/completion의 소유자 각 1→1. 신규 정책 상태·queue·lock·timer 0. 복원 범위 밖 차이 0.

검증 근거와 commit은 [report.md](report.md)에 기록한다. 재개 후 L4(4) 대 L5_7(7), send-saturation·request-serial·request-backpressure × 1024/4096 B의 6셀을 비교한다. 묶음 예산은 21%다.
