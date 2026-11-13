# C++ L3 (#4353) 호출 대조

작업 경로 `/root/worktree/zlink-v21-s033/l3868-next`, 이전 단계 L2 `fbdcb16e26`. 측정하지 않는다. `B=framework/bench/grpc/cpp`, `R=framework/languages/cpp/framework/src/runtime`. 아래 line은 L3 소스 기준이다. L2 커밋의 호출 대조는 [call-shape-L2.md](call-shape-L2.md)가 보존한다.

| 경로·작업 | binding file:line | L3 file:line | 동일 여부·복원 기능 |
|---|---|---|---|
| source payload 생성·stamp·새 protobuf body | `B/client/bench_cpp_client.cpp:447,461` | `B/ladder/ladder_wire.hpp:127` | 동일 작업·같은 encode_bench_payload; typed codec 미복원 |
| source envelope header | `B/client/bench_cpp_client.cpp:459` 상수 header | `B/ladder/ladder_wire.hpp:136,144` | L2 envelope·correlation·deadline 유지; L3 request는 production fused service encoder 사용 |
| source service header·multipart | 없음 | `B/ladder/ladder_wire.hpp:148` | L3: encode_node_send/request_header_message + encode_application_payload_message; inner 2-part 순서 보존 |
| source 2-part send/request | `B/client/bench_cpp_client.cpp:465,470` | `B/ladder/ladder_driver.inc:121,129` | 같은 binding API·admitted/reply observer·timeout; part 수 2개 유지 |
| target permit | 없음 | `R/mesh/raw_mesh_node_owner.cpp:535` | L1 실제 permit 유지; handler 진입 `B/ladder/ladder_wire.hpp:384`에서 반환 |
| target blocking receive·오류 처리 | `B/zlink/bench_zlink_cpp_server.cpp:41,84` | `R/mesh/raw_mesh_node_owner.cpp:526,541` | 같은 recv(none)·stop.load·실패 시 50us sleep; 추가 readiness wait/drain 없음 |
| target service header 검사 | 없음 | `B/ladder/ladder_wire.hpp:59,85` | L3: 수신 경계의 production decode_header/decode_node_request_header; correlation은 reply로 전달하고 재파싱하지 않음 |
| target multipart decode·소유권 | 없음 | `B/ladder/ladder_wire.hpp:235` | L3: production decode_application_parts(move(frame)); 원본 bytes를 retained Message slices로 이전, payload bytes copy 0 |
| target envelope parse | 없음 | `B/ladder/ladder_wire.hpp:241,253` | L2 parser 1회; L3는 production retained_header 필드로 원본 header 수명을 reply까지 보존 |
| target protobuf parse·metrics | `B/zlink/bench_zlink_cpp_server.cpp:54,56,97,99` | `B/ladder/ladder_wire.hpp:247,250` | 동일 ParseFromArray·metrics.record; 고정 handler 직접 호출(L0) |
| target 새 reply body | `B/zlink/bench_zlink_cpp_server.cpp:61` | `B/ladder/ladder_wire.hpp:263` | 동일 새 payload·set_body copy·protobuf encode·새 Message; request part 재사용 없음 |
| target 새 response envelope | `B/zlink/bench_zlink_cpp_server.cpp:58` | `B/ladder/ladder_wire.hpp:268` | L2 production writer 유지; request envelope 재파싱 없음 |
| target service reply wrapper | 없음 | `B/ladder/ladder_wire.hpp:288` | L3: 기존 correlation으로 production reply header + fresh multipart payload encode |
| target 2-part reply submit | `B/zlink/bench_zlink_cpp_server.cpp:67` | `B/ladder/ladder_wire.hpp:292` | 같은 received.reply().message().message().submit() |
| source completion wait | `B/client/bench_cpp_client.cpp:622` | `B/ladder/ladder_driver.inc:40` | 같은 binding POLLCOMPLETION wait; 별도 completion worker/registry 없음 |
| source completion service decode | 없음 | `B/ladder/ladder_wire.hpp:301,310` | L3: production decode_reply_header·decode_application_part_views; exact generic header 크기·correlation·terminal 검사 |
| source completion envelope | 없음 | `B/ladder/ladder_wire.hpp:316` | L2 header parse 1회·production error mapper 유지 |
| source completion protobuf·counter | `B/client/bench_cpp_client.cpp:475,479,482` | `B/ladder/ladder_wire.hpp:324`, `B/ladder/ladder_driver.inc:135,142` | 동일 protobuf ParseFromArray·stream.finish/admit; 수동 field shortcut 없음 |

Production 근거:

- source: `R/channels/channel_runtime.cpp:1898,1920` → `R/messaging/client_call_codec.cpp:83`(fused request); `R/mesh/raw_mesh_node_owner.cpp:1412,1466,1910`(node header + application payload pack).
- target: `R/mesh/raw_mesh_node_owner.cpp:4257,4267`(node header validation); `R/stateful/public_host_runtime.cpp:8418` → `R/protocol/service_wire_codec.cpp:3400`(실제 node ingress retained Message decoder); `R/mesh/mesh_node_host_service.cpp:3809,148`(borrowed envelope parse); `R/mesh/mesh_record_dispatcher.cpp:47`(retained header transfer).
- reply: `R/channels/route_packet_dispatcher.cpp:444`; `R/mesh/raw_mesh_node_owner.cpp:2182`(service reply header + payload pack).
- completion: `R/mesh/raw_mesh_node_owner.cpp:1567`(generic reply header); `R/mesh/mesh_node_runtime.cpp:4406`(borrowed multipart views); `R/channels/channel_runtime.cpp:1822`(envelope parse).

| 정상 메시지당 비용 | binding / L0 | L1/L2 유지 | L3 추가 |
|---|---|---|---|
| wait·metrics | blocking recv·binding completion wait·동일 metrics | permit 예약/반환 및 envelope 처리 | 추가 wait/drain/metrics atomic 0 |
| source payload | raw protobuf encode·새 Message | payload 소유권 move | service frame pack의 byte write/copy; request는 production fused writer로 header를 frame에 직접 기록 |
| target payload | raw protobuf decode | header 1회 parse | native slice vector·retained frame owner·slice refcount; application bytes copy 0 |
| reply | 새 body copy·protobuf encode·새 parts·2-part submit | 새 production response header | production service header + multipart encode; request parts 재사용 0 |
| completion payload | raw protobuf decode·stream.finish | header parse 1회 | service header·multipart span vector; payload byte copy 0 |

L3의 correlation은 수신 경계가 해석한 stack 값 하나이며 registry/timeout queue를 추가하지 않는다. Header 보존은 기존 envelope_header_t.retained_header를 재사용한다. 수신의 이전 span vector는 retained Message vector로 바꿨다. Framework parser 판정 위치는 target 2→1이며 service framing 형식은 기존 service_wire_codec 한 곳(1→1)이 소유한다. Completion은 operation registry/request storage(L7)를 복원하지 않았으므로 decoder의 표준 vector overload(`R/protocol/service_wire_codec.cpp:3365`)를 사용한다. Main의 `mesh_node_runtime.cpp:4406`은 같은 decode kernel의 PMR overload를 request storage로 호출한다. 이 container allocation은 L3 framing 비용으로 기록하고 L7 allocator 경로를 앞당겨 추가하지 않는다. Core/native 내부 allocation 총계는 추정하지 않는다.

검증·커밋 결과는 완료 후 추가한다. 복원 범위 밖 차이는 커밋 전에 0이어야 한다.
