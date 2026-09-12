# C++ send 건당 비용 — 조사 결과

조사 범위는 `send-saturation`의 source A와 target B다. 벤치 구현 표에서
`zlink-framework-cpp`는 `bench_cpp_client`의 `route_client_t`와
`bench_cpp_framework_server`를, `zlink-cpp`는 같은 client의 raw ROUTER와
`bench_cpp_zlink_server`를 사용한다
(`framework/bench/grpc/doc/cpp.ko.md:11-16`, `:59-60`). 벤치는 실행하지 않았다.

## 1. send 한 건이 지나는 경로

### Framework source A — `zlink-framework-cpp`

`send-saturation`은 `framework_driver_t::submit<void>`에서
`route_client_t::send_to_node("bench", target, payload).async()`를 호출한다
(`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:872-881`, `:909-927`).
실제 경로는 다음과 같다.

1. `route_client_t::send_to_node`가 payload를 `shared_ptr<TMessage>`에 옮기고 typed
   serializer closure를 만든다
   (`framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:773-795`).
2. `route_send_call_t::async`가 single-use claim 뒤 submit coroutine을 await한다
   (`framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:1286-1305`).
3. `submit_send_erased`가 flow scope, Mesh sender lookup을 거쳐 protobuf serializer와
   Framework JSON envelope을 만든다
   (`framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:1333-1393`).
   protobuf serializer는 `message_t::allocate(size)` 뒤 `SerializeToArray`로 body를 쓴다
   (`framework/languages/cpp/extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:21-34`);
   envelope encoder는 크기를 한 번 재고 header message를 할당해 두 번째 pass로 쓴다
   (`framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:310-332`).
4. Mesh sender lookup은 `channel_runtime_state_t::lane.run(...).get()`으로 map을 읽고
   (`framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:61-69`),
   host가 등록한 sender가 `mesh_node_runtime_t::send_to_node`으로 전달한다
   (`framework/languages/cpp/framework/src/runtime/host/app.cpp:1979-1993`).
5. `mesh_node_runtime_t::send_to_node`는 먼저 Framework envelope을 decode해 internal
   packet인지 검사한 뒤, public host runtime으로 전달한다
   (`framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:284-294`,
   `:4102-4115`). `public_host_runtime_t`는 두 Framework part를 service-wire
   `application_payload_t::from_parts`로 바꾸어 raw mesh transport에 넘긴다
   (`framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:2643-2649`,
   `:6100-6105`).
6. `raw_mesh_node_owner_t::send_to_node_result`가 `nodeSend` header를 만들고
   `send_with_header_result`로 간다
   (`framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1089-1096`).
   여기서 multipart payload를 service-wire byte vector로 다시 encode하고, send completion
   source를 만든다 (`:1497-1522`).
7. `start_send`가 owner state lane turn 안에서 operation registry에 operation/callback을
   등록하고 raw port task를 만든 뒤, task completion observer로 registry completion을
   확정한다 (`framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1396-1494`).
8. `raw_route_port_t::send_result`가 socket mutex 안에서 byte vector를 external
   `message_t`로 materialize하고 raw binding `router_socket_t::send(rid)...async()`를
   호출한다 (`framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:185-271`).
   binding의 즉시-admitted path는 최종적으로 `zlink_send_rid` 한 번을 호출한다
   (`bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:101-138`).

### Raw source A — `zlink-cpp`

1. 같은 bench client가 `make_parts`에서 상수 JSON header와 protobuf bytes를 두
   `message_t`로 만든다 (`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:602-617`).
2. `send_slot`이 `router_socket_t::send(target).message(header).message(body).async()`를
   호출하고, backpressure일 때만 binding admission task를 await한다
   (`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:727-750`).
3. binding `send_submit_operation_t::async`는 DONTWAIT를 설정하고
   `submit_send_awaitable`로 들어간다
   (`bindings/cpp/src/Runtime/Messaging/send_operations.cpp:168-175`). 즉시 수락이면
   completion entry나 waiter map을 만들지 않고 source를 해제한 뒤 ready result를 반환한다
   (`bindings/cpp/src/Runtime/Messaging/send_operations.cpp:48-84`), 그리고
   `zlink_send_rid`만 실행한다
   (`bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:101-138`).

### Framework target B — `zlink-framework-cpp`

1. raw port가 socket mutex 안에서 `recv(DONTWAIT)`하고 binding-owned 두 outer frame을
   Framework byte vectors로 복사한다
   (`framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:449-481`,
   `framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp:43-55`).
2. raw mesh pump가 service-wire header를 decode하고 node send를 application mailbox에
   넣는다 (`framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:2979-3035`,
   `:3841-3961`). mailbox enqueue는 mutex, owner queue, byte/message accounting과 ready
   notification을 수행한다 (`framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:40-69`).
3. public host dispatch가 outer application payload를 decode하고, contained Framework
   multipart를 새 `message_t`들로 복원한다
   (`framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:5903-5911`,
   `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:2982-3017`,
   `:3123-3149`).
4. mesh record dispatcher가 Framework envelope header를 decode하고 route dispatcher로
   넘긴다 (`framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:29-82`).
5. route dispatcher가 command body를 꺼내 handler invoker를 실행한다
   (`framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:139-215`).
   invoker는 handler scope를 만들고 body를 `message.copy()`한 closure로 넘기며
   (`framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.cpp:22-38`),
   typed handler closure가 protobuf deserialize 후 `command_handler_t::handle`을 호출한다
   (`framework/languages/cpp/framework/src/runtime/channels/route_handler_registry.hpp:50-65`,
   `framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp:28-35`).

### Raw target B — `zlink-cpp`

`command_loop`은 raw ROUTER receive 뒤 두 part만 확인하고, 이미 binding이 보유한 body를
`BenchPayload::ParseFromArray`에 직접 넣어 metric을 기록한다
(`framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:78-105`). Service-wire decode,
mailbox, Framework envelope decode, handler scope와 body copy는 없다.

## 2. 지목한 비용, 비싼 순서로

아래 순서는 profiler 수치가 아니라 1 KiB body에 대해 코드상 수행하는 전체-body copy/serialize
횟수와 동기화·heap 객체 수로 정한 우선순위다. 정확한 비중은 다음 perf ticket에서 확인해야 한다.

### 1. Framework multipart를 service-wire로 재포장하고 target에서 다시 푸는 전체-body 복사

- **무엇** — `[Framework JSON header, protobuf body]`를 `application_payload_t`로 복사하고
  contiguous service-wire byte frame으로 serialize한 뒤, target에서 byte vector와 새 part들로
  다시 복원한다.
- **어디** — source copy는
  `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:2967-2979`,
  source wire encode는 `:3062-3121`; target outer-frame copy는
  `framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp:43-55`,
  target payload decode와 part 재생성은
  `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:2982-3017`,
  `:3123-3149`.
- **왜 비싼가** — `from_parts`가 두 part 각각에 `part.copy()`를 수행한다. 이어
  `encode_application_payload`가 모든 part bytes를 새 vector에 `insert`한다. target은 binding
  part를 한 번 복사하고, `decode_application_payload`가 inner multipart 전체를 새 vector에
  `assign`하며, `decode_application_parts`가 각 inner part를 새 `message_t`로 만든다. 1 KiB body는
  protobuf 직렬화 뒤에도 source에서 최소 두 전체 copy, target에서 최소 세 전체 copy를 지난다.
  raw target은 binding body를 그대로 protobuf parser에 전달한다.
- **raw는 어떻게 하는가** — raw source는 header/body 두 `message_t`를 바로 binding builder에
  넘긴다 (`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:727-739`); raw target은 receive
  body에 `ParseFromArray`만 한다
  (`framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:90-100`).
- **고치는 방법** — RouteMesh service-wire의 bytes와 multipart profile은 유지하되, Framework
  part를 `application_payload_t`에 복사해 넣지 말고 immutable ownership bundle로 유지한다.
  binding submit에서 outer frame 하나만 만들도록 scatter/gather 또는 Core가 수용하는 owned
  multipart record를 사용하고, target에서도 binding receive lifetime을 mailbox reservation까지
  보존해 outer byte vector와 inner `message_t` 재생성을 없앤다. Core/binding이 single contiguous
  frame만 받을 수 있으면 그 계약을 넓혀야 한다.
- **위험** — binding receive lifetime, mailbox shutdown/drain, HWM byte accounting과 cross-language
  service-wire bytes가 함께 바뀐다. borrowed frame이 handler 뒤까지 남으면 close·재사용 오류가
  생길 수 있다.

### 2. 즉시 수락된 send까지 operation registry와 completion graph를 만드는 경로

- **무엇** — one-way send가 reply를 기다리지 않아도 owner operation ID, registry callback,
  `task_completion_source`, `shared_ptr<task_t<...>>`, completion observer를 만든다.
- **어디** —
  `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1390-1494`,
  `:1497-1522`; binding의 즉시 수락 분기는
  `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:48-84`.
- **왜 비싼가** — 매 message마다 `send_completion_state_t`와 source를 heap에 만들고,
  `start_send`가 registry entry/callback과 heap `task_t`를 만든다. binding이
  `ZLINK_SUBMIT_OK`이면 `raw_route_port_t`는 ready task를 즉시 반환한다
  (`framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:228-261`). 그런데도
  Framework는 observer callback으로 registry `complete`를 수행하고, 그 callback이 다시 public
  task를 완료한다. raw binding은 이 경우 completion entry·waiter-map node도 만들지 않는다.
- **raw는 어떻게 하는가** — `submit_send_awaitable`은 admitted path에서 native submit 뒤 source를
  detach/release하고 즉시 ready result만 반환한다
  (`bindings/cpp/src/Runtime/Messaging/send_operations.cpp:77-84`).
- **고치는 방법** — raw port가 `immediately_admitted`와 `pending_backpressure`를 구분해 반환하게
  하고, immediate case는 lifecycle-safe port snapshot 후 public `task_t<void>`를 바로 완료한다.
  registry, completion source, observer는 binding이 backpressured여서 실제 admission completion을
  기다릴 때만 만든다. public completion boundary는 여전히 binding의 local admission 결과다.
- **위험** — shutdown과 socket close가 submit 직전/직후에 겹칠 때 terminal-once 보장이 깨질 수
  있다. pending branch의 operation registry 제거까지 넓히면 timeout·HWM 재제출 및 drain의
  소유자가 사라진다.

### 3. source의 두 state-lane turn과 socket mutex

- **무엇** — 매 direct send에서 sender map lookup과 raw mesh send start를 각각
  `state_lane_t::run(...).get()`으로 직렬화하고, 마지막 submit은 socket mutex를 획득한다.
- **어디** — sender lookup
  `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:61-69`; mesh owner
  turn `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1408-1462`;
  lane의 promise allocation, mailbox mutex와 FIFO drain은
  `framework/languages/cpp/framework/src/runtime/execution/state_lane.hpp:117-155`,
  `framework/languages/cpp/framework/src/runtime/execution/state_lane.cpp:75-139`; socket mutex는
  `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:217-238`.
- **왜 비싼가** — `run`은 매 호출에 `shared_ptr<promise>`와 `std::function`을 만들고 mutex로
  mailbox에 넣어 future `.get()`을 한다. source send 하나가 이 turn을 둘 통과하며, 두 번째
  turn 안에서는 registry 작업도 한다. 8 task slot이 같은 MeshNode와 socket을 공유하므로 이
  고정 비용은 contention까지 만든다.
- **raw는 어떻게 하는가** — raw bench slot은 바로 binding builder를 호출한다
  (`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:727-745`). binding admitted path는
  per-operation completion registry를 만들지 않는다
  (`bindings/cpp/src/Runtime/Messaging/send_operations.cpp:48-84`).
- **고치는 방법** — immutable sender/port handle을 startup에 publish하고 fast path는 atomic
  snapshot으로 읽는다. lifecycle state를 실제로 바꾸는 close/connect와 pending-backpressure
  등록만 owner lane에 남긴다. socket mutex는 하나만 남겨 Core socket ownership을 보존한다.
- **위험** — port pointer의 lifetime과 `close()`의 race를 reference-counted snapshot으로 정확히
  막아야 한다. lane 밖에서 topology 또는 mutable lifecycle state를 읽으면 안 된다.

### 4. target handler 전달 전의 추가 body copy와 scope/queue bookkeeping

- **무엇** — target은 service mailbox와 route dispatcher를 거친 뒤 handler closure용
  `message.copy()`와 invocation scope를 추가로 만든다.
- **어디** — mailbox accounting
  `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:40-69`; route handler
  body dispatch `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:171-215`;
  scope와 body copy
  `framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.cpp:22-38`; typed
  protobuf decode `framework/languages/cpp/framework/src/runtime/channels/route_handler_registry.hpp:50-65`.
- **왜 비싼가** — `message.copy()`는 1 KiB body를 다시 복사한다. 그 전후로 owner queue의 mutex,
  map/deque, retained byte count, scope object, filter continuation task가 message마다 동작한다.
  raw target에는 queue/scope가 없고 protobuf parse 뒤 metric handler를 직접 호출한다.
- **raw는 어떻게 하는가** — raw command loop의 part-count 검사, `ParseFromArray`, `metrics.record`
  뿐이다 (`framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:89-100`).
- **고치는 방법** — filter/handler API의 value lifetime을 유지하는 범위에서 invoker closure가
  mailbox claim의 owned body를 이동해 사용하게 하고 `message.copy()`를 제거한다. singleton
  handler의 scope creation도 no-filter/no-scoped-service fast path로 분리할 수 있다.
- **위험** — filter가 continuation을 저장하거나 handler coroutine이 suspend하면 message lifetime이
  claim보다 길어진다. scope 생략은 scoped DI 서비스의 격리를 깨뜨릴 수 있다.

### 5. Framework envelope을 source에서 두 번 해석하고 target에서 다시 해석

- **무엇** — source가 outbound JSON envelope을 만들고, Mesh runtime이 internal packet 여부를
  알아보려고 이를 바로 decode하며, target dispatcher가 같은 envelope을 다시 decode한다.
- **어디** — create/envelope encode
  `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:1361-1381`,
  `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:310-332`; source-side
  decode `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:284-294`; target
  decode `framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:43-65`.
- **왜 비싼가** — every application send allocates a correlation-id string
  (`framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp:14-27`) and header
  message, then `framework_owned_node_message` parses it only to test a reserved name prefix. Target도
  routing/typed dispatch를 위해 같은 header를 parse한다. Raw benchmark header는 상수 string이고
  source에서 parse하지 않는다 (`framework/bench/grpc/cpp/common/bench_common.hpp:154-161`).
- **raw는 어떻게 하는가** — raw source는 constant header bytes를 만들고 binding에 전달하며,
  raw command target은 envelope을 해석하지 않는다
  (`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:606-616`,
  `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:89-100`).
- **고치는 방법** — `message_parts_t`에 private, non-wire `framework_owned` tag를 붙여 source
  internal-packet 판별에 decode를 쓰지 않게 한다. target header decode와 wire JSON 형식은 유지한다.
  direct application send에 correlation ID가 공개 계약상 필요 없는지도 별도 계약 검토 후,
  필요 없다면 creation을 피한다.
- **위험** — tag가 frame 이동·copy·local submit에서 소실되면 internal packet이 잘못 분류된다.
  correlation ID 제거는 diagnostics/flow와 peer interoperability의 공개 의미를 바꿀 수 있다.

## 3. 고치지 말아야 할 것

- **send를 remote handler 완료까지 기다리는 request식 completion으로 바꾸지 않는다.** node direct
  send는 source-local queue 수락으로 반환 데이터 없이 완료하고
  (`framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md:19-26`),
  C++ one-way `async()`도 remote handler 완료를 기다리지 않는다고 명시한다
  (`framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:1032-1039`).
  §2의 completion graph 제거는 이 경계를 더 정확히 구현하는 범위여야 한다.

- **`nodeSend` service-wire command와 Framework multipart profile의 wire 형식을 없애거나 raw
  direct-handler 경로로 바꾸지 않는다.** `nodeSend`는 application payload가 필수인 RouteMesh
  application command이고
  (`framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md:219-230`),
  multipart profile은 part 순서와 opaque bytes 보존을 요구한다
  (`:183-189`). 최적화 대상은 이 형식 자체가 아니라 형식 사이의 불필요한 materialization이다.

- **target mailbox/handler turn을 receive thread의 즉시 handler 호출로 우회하지 않는다.** application
  record는 permit 뒤 owner mailbox 또는 serial queue handler turn으로 이전해야 하며
  (`framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:114-123`),
  queue가 찼다고 호출 위치에서 실행하면 직렬 실행 전제가 깨진다
  (`framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:266-280`).

- **Core가 소유한 DONTWAIT/HWM 재시도와 pending binding completion을 Framework에서 다시 구현하지
  않는다.** raw binding은 immediate attempt 뒤 backpressure일 때만 completion entry를 만들고
  (`bindings/cpp/src/Runtime/Messaging/send_operations.cpp:48-119`), Framework 계약도 binding
  operation 시작 뒤 HWM 재시도와 completion은 Core가 소유한다고 한다
  (`framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md:168-172`).

## 4. 확인 못 한 것

- 각 후보의 실제 CPU 비중과 위 우선순위가 perf profile에서 유지되는지는 측정하지 않았다.
- `state_lane_t::run`이 이 benchmark process에서 항상 caller thread에서 drain되는지, 또는 executor
  worker 경쟁이 실제로 생기는지는 runtime trace/profile 없이 확정하지 않았다.
- `framework_owned_node_message` 뒤의 `classify_node_direct_target`이 bench source에서
  Location Store를 실제 조회하는지는 `_user_spot_store` 설정과 live topology를 실행하지 않아
  확인하지 못했다. source-side envelope decode 자체는 조건과 무관하게 확인했다.
- Core가 service-wire outer frame의 scatter/gather 또는 retained receive ownership을 지원하는지는
  bindings/cpp 공개 surface만 읽어서 확정할 수 없다. 따라서 §2-1의 구체적 zero-copy 방안은
  Core/binding 계약 조사와 별도 설계 승인이 필요하다.
