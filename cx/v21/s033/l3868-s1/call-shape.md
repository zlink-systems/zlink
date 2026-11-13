# C++ binding / L0~L5~L7 호출 대조

HEAD `62e766f88dc6d1feefdff76e6132185019db0bb6`, clean. 아래 line은 현재 tree 기준이다.
B=`framework/bench/grpc/cpp`, R=`framework/languages/cpp/framework/src/runtime`.
Binding과 ladder의 공통 counters·body stamp·payload 크기·request timeout·2-part submit을 유지한다.

| 단계 | 경로 | binding 호출(file:line) | ladder 호출(file:line) | 동일 여부 / 차이의 복원 기능 |
|---|---|---|---|---|
| L0 | source send | `B/client/bench_cpp_client.cpp:455,465,470` | `B/ladder/ladder_driver.inc:115 → 같은 submit_raw` | 동일 payload 생성·protobuf encode·2-part binding send/request·observer |
| L0 | target receive | `B/zlink/bench_zlink_cpp_server.cpp:41,54,56,84,97,99` | `R/mesh/raw_mesh_node_owner.cpp:526,535,539,541; B/ladder/ladder_wire.hpp:264,267,270,412` | 같은 blocking recv(NONE), drain 0, protobuf parse·동일 metrics; L0 고정 handler 직접 호출 |
| L0 | target reply | `B/zlink/bench_zlink_cpp_server.cpp:58,61,63,67` | `B/ladder/ladder_wire.hpp:278,283,288,298,308` | 새 envelope Message·새 protobuf body copy/encode·새 Message·같은 2-part reply |
| L0 | source completion | `B/client/bench_cpp_client.cpp:473,475,479,622` | `B/ladder/ladder_driver.inc:40,117 → 같은 submit_raw` | 동일 POLLCOMPLETION wait·binding reply/admission observer·protobuf parse·finish/admit |
| L1 | source send | `B/client/bench_cpp_client.cpp:455,465,470` | `B/ladder/ladder_driver.inc:115 → 같은 submit_raw` | 동일 payload 생성·protobuf encode·2-part binding send/request·observer |
| L1 | target receive | `B/zlink/bench_zlink_cpp_server.cpp:41,54,56,84,97,99` | `R/mesh/raw_mesh_node_owner.cpp:526,535,539,541; B/ladder/ladder_wire.hpp:264,267,270,412` | 같은 blocking recv(NONE), drain 0, protobuf parse·동일 metrics; L0 고정 handler 직접 호출; L1 수신 전 permit 획득·handler 직전 반환 |
| L1 | target reply | `B/zlink/bench_zlink_cpp_server.cpp:58,61,63,67` | `B/ladder/ladder_wire.hpp:278,283,288,298,308` | 새 envelope Message·새 protobuf body copy/encode·새 Message·같은 2-part reply |
| L1 | source completion | `B/client/bench_cpp_client.cpp:473,475,479,622` | `B/ladder/ladder_driver.inc:40,117 → 같은 submit_raw` | 동일 POLLCOMPLETION wait·binding reply/admission observer·protobuf parse·finish/admit |
| L2 | source send | `B/client/bench_cpp_client.cpp:455,461,465,470` | `B/ladder/ladder_wire.hpp:126,132,146,151,156; B/ladder/ladder_driver.inc:121,129` | L2 요청 envelope encode; protobuf encode 유지; 같은 binding submit/observer |
| L2 | target receive | `B/zlink/bench_zlink_cpp_server.cpp:41,54,56,84,97,99` | `R/mesh/raw_mesh_node_owner.cpp:526,535,541; B/ladder/ladder_wire.hpp:253,259,264,267,270,412` | 같은 blocking recv·drain 0·metrics와 L1 permit; L2 header parse 1회; protobuf parse 동일 |
| L2 | target reply | `B/zlink/bench_zlink_cpp_server.cpp:58,61,63,67` | `B/ladder/ladder_wire.hpp:278,283,286,292,308,311,315` | 새 body copy·새 parts·같은 2-part submit; L2 response envelope encode; 같은 protobuf encode |
| L2 | source completion | `B/client/bench_cpp_client.cpp:473,475,479,622` | `B/ladder/ladder_driver.inc:40,132,136,139; B/ladder/ladder_wire.hpp:324,341,347,350` | 같은 binding wait/observer/counters; L2 response header parse 1회; 같은 protobuf parse/finish |
| L3 | source send | `B/client/bench_cpp_client.cpp:455,461,465,470` | `B/ladder/ladder_wire.hpp:126,132,146,151,156; B/ladder/ladder_driver.inc:121,129` | L2 요청 envelope encode; L3 service header/multipart pack; protobuf encode 유지; 같은 binding submit/observer |
| L3 | target receive | `B/zlink/bench_zlink_cpp_server.cpp:41,54,56,84,97,99` | `R/mesh/raw_mesh_node_owner.cpp:526,535,541; B/ladder/ladder_wire.hpp:253,259,264,267,270,412` | 같은 blocking recv·drain 0·metrics와 L1 permit; L2 header parse 1회; L3 retained multipart unpack; protobuf parse 동일 |
| L3 | target reply | `B/zlink/bench_zlink_cpp_server.cpp:58,61,63,67` | `B/ladder/ladder_wire.hpp:278,283,286,292,308,311,315` | 새 body copy·새 parts·같은 2-part submit; L2 response envelope encode; L3 service reply/multipart pack; 같은 protobuf encode |
| L3 | source completion | `B/client/bench_cpp_client.cpp:473,475,479,622` | `B/ladder/ladder_driver.inc:40,132,136,139; B/ladder/ladder_wire.hpp:324,341,347,350` | 같은 binding wait/observer/counters; L2 response header parse 1회; L3 service/multipart unpack; 같은 protobuf parse/finish |
| L4 | source send | `B/client/bench_cpp_client.cpp:455,461,465,470` | `B/ladder/ladder_wire.hpp:126,132,146,151,156; B/ladder/ladder_driver.inc:121,129` | L2 요청 envelope encode; L3 service header/multipart pack; L4 기본 typed JSON serialize (binary field base64); 같은 binding submit/observer |
| L4 | target receive | `B/zlink/bench_zlink_cpp_server.cpp:41,54,56,84,97,99` | `R/mesh/raw_mesh_node_owner.cpp:526,535,541; B/ladder/ladder_wire.hpp:253,259,264,267,270,412` | 같은 blocking recv·drain 0·metrics와 L1 permit; L2 header parse 1회; L3 retained multipart unpack; L4 typed JSON deserialize |
| L4 | target reply | `B/zlink/bench_zlink_cpp_server.cpp:58,61,63,67` | `B/ladder/ladder_wire.hpp:278,283,286,292,308,311,315` | 새 body copy·새 parts·같은 2-part submit; L2 response envelope encode; L3 service reply/multipart pack; L4 typed JSON serialize |
| L4 | source completion | `B/client/bench_cpp_client.cpp:473,475,479,622` | `B/ladder/ladder_driver.inc:40,132,136,139; B/ladder/ladder_wire.hpp:324,341,347,350` | 같은 binding wait/observer/counters; L2 response header parse 1회; L3 service/multipart unpack; L4 typed JSON deserialize/finish_bytes |
| L5~L7 | source send | `B/client/bench_cpp_client.cpp:455,461,465,470` | `B/client/bench_cpp_client.cpp:713,726,728,734; R/channels/channel_runtime.cpp:1481,1898` | L4 JSON 유지; L5 public owner submit; L7 operation identity/registry/deadline/admission |
| L5~L7 | target receive | `B/zlink/bench_zlink_cpp_server.cpp:41,54,56,84,97,99` | `B/framework/bench_framework_cpp_server.cpp:91,97,99; R/mesh/mesh_node_host_service.cpp:2163,2299,3869; R/dispatch/offload_executor.cpp:427; R/channels/route_handler_invoker.cpp:37` | L5 production readiness/receive rotation/drain/mailbox/worker; L6 dispatch별 handler activation·DI scope; 기존 codec·metrics 유지 |
| L5~L7 | target reply | `B/zlink/bench_zlink_cpp_server.cpp:58,61,63,67` | `B/framework/bench_framework_cpp_server.cpp:26,28,29; R/channels/route_handler_registry.hpp:147; R/channels/route_packet_dispatcher.cpp:426,444; R/mesh/raw_mesh_node_owner.cpp:2177` | 새 body copy·새 JSON encode·response envelope·2-part service reply 유지; L6 invocation/scope·L7 completion dispatcher |
| L5~L7 | source completion | `B/client/bench_cpp_client.cpp:473,475,479,622` | `B/client/bench_cpp_client.cpp:736,737,741; R/foundation/operation_registry.cpp:270,382,735; R/dispatch/provider_task_context.cpp:172; R/mesh/mesh_node_runtime.cpp:4383` | L7 registry/terminal/deadline/completion turn·request storage; L4 JSON decode와 동일 finish_bytes/admit |

정상 메시지당 비용: binding/L0는 blocking recv 1회·drain 0, source completion poll wait,
source payload 1개·protobuf encode·새 header/body Message, target protobuf decode·동일 metrics,
request reply는 새 envelope·새 body copy·protobuf encode·새 Message·2-part submit,
completion protobuf decode·같은 stream counters다. L0 추가 wait/metrics atomic/body copy는 없다.
L1만 permit 예약/반환(credits load 2·fetch_add 1·fetch_sub 1 및 수명 refcount)을 더한다.
L2는 request/response envelope encode·각 1회 parse·production parts container·request correlation fetch_add를 더한다.
L3는 service pack의 byte write/copy·retained slice/vector/refcount·completion span vector를 더하며 target/completion payload byte copy는 0이다.
L4는 protobuf 대신 기본 JSON/base64 typed codec의 allocation/encode/decode를 사용한다. 추가 wait/drain/metrics atomic은 없다.
L5~L7은 public production owner/worker/activation/scope/registry/deadline/completion 경로와 그 소유 allocation/atomic을 복원한다.
Core/binding 내부 allocation·atomic 총수는 계측하지 않아 추정하지 않는다.

복원 기능에 속하지 않는 호출 차이 0. Control-plane admission은 active 시작 전 완료한다.
L5~L7의 수신 방식 변경은 복원된 L5 production receive/worker 기능에 속한다.
상세 호출 근거: `../l3868-impl2/call-shape-L2.md`, `../l3868-impl2/call-shape-L3.md`,
`../l3868-impl3/call-shape-L4.md`, `../l3868-impl3/call-shape-L5-7.md`. 코드·계약 변경 없음.
