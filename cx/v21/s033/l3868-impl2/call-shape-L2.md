# C++ L2 (#4352) 호출 대조

작업 경로 `/root/worktree/zlink-v21-s033/l3868-next`, 시작 HEAD `a67753ede4`, L1 기준 `608513f260`. 측정하지 않는다. 아래 행은 L2 구현 소스 기준이며 커밋 전 빌드·계약 테스트로 확인한다. `B=framework/bench/grpc/cpp`, `R=framework/languages/cpp/framework/src/runtime`.

| 경로·작업 | binding file:line | L2 file:line | 동일 여부·복원 기능 |
|---|---|---|---|
| source payload 생성·stamp·protobuf encode·새 body | `B/client/bench_cpp_client.cpp:447,459` | `B/ladder/ladder_wire.hpp:106` | 동일 작업·같은 `encode_bench_payload`; typed codec 복원 없음 |
| source 새 header | `B/client/bench_cpp_client.cpp:455` 상수 envelope 복사 | `B/ladder/ladder_wire.hpp:114` create_envelope → encode_raw_body_parts | L2: 실제 Framework command/request header; request correlation·deadline 포함 |
| source 2-part send/request·binding observer | `B/client/bench_cpp_client.cpp:465,471,472,482` | `B/ladder/ladder_driver.inc:119,128,131,142` | 같은 binding API·같은 admitted/reply observer·stream finish/admit |
| target 수신 전 permit | 없음 | `R/mesh/raw_mesh_node_owner.cpp:535` | L1 유지: 실제 application job queue 예약 |
| target recv·오류 처리 | `B/zlink/bench_zlink_cpp_server.cpp:41,84` | `R/mesh/raw_mesh_node_owner.cpp:526,541` | 같은 blocking recv(none), stop.load, 실패 시 50us sleep; readiness wait·drain 추가 없음 |
| target header parse | 없음 | `B/ladder/ladder_wire.hpp:210` → `R/messaging/envelope_codec.cpp:1301` | L2: decode_header_view 1회; 입력 header를 빌리고 파싱 결과를 reply까지 보존 |
| target protobuf parse·metrics | `B/zlink/bench_zlink_cpp_server.cpp:52,54,95,97` | `B/ladder/ladder_wire.hpp:219,222` | 동일 protobuf ParseFromArray·metrics.record; payload 원본 Message/span 유지 |
| target handler 진입 | 직접 실행 | `B/ladder/ladder_wire.hpp:350` | L0 고정 handler 직접 호출, L1 permit release 유지; worker·DI 추가 없음 |
| target 새 reply envelope | `B/zlink/bench_zlink_cpp_server.cpp:58` | `B/ladder/ladder_wire.hpp:237` → `R/channels/channel_reply_writer.cpp:12,41` | L2: 이미 파싱한 request로 response header 생성·encode; 재파싱 없음 |
| target 새 reply protobuf body | `B/zlink/bench_zlink_cpp_server.cpp:61` | `B/ladder/ladder_wire.hpp:232` | 동일 새 payload·set_body 복사·protobuf encode·새 Message; request part 재사용 없음 |
| target 2-part reply submit | `B/zlink/bench_zlink_cpp_server.cpp:67` | `B/ladder/ladder_wire.hpp:253` | 같은 received.reply().message().message().submit() |
| source completion wait | `B/client/bench_cpp_client.cpp:622` | `B/ladder/ladder_driver.inc:40` | 같은 binding POLLCOMPLETION wait; owner queue·registry 추가 없음 |
| source completion header | 없음 | `B/ladder/ladder_wire.hpp:286` | L2: 실제 envelope.decode_header 1회·실제 error mapper |
| source completion protobuf·counter | `B/client/bench_cpp_client.cpp:474` | `B/ladder/ladder_wire.hpp:294`, `B/ladder/ladder_driver.inc:134` | 동일 protobuf ParseFromArray·stream.finish·admit; 수동 protobuf field shortcut 제거 |

생산 호출 근거: source `R/channels/channel_runtime.cpp:1652` → `R/messaging/client_call_codec.cpp:94`; target `R/mesh/mesh_node_host_service.cpp:2326,148`; reply `R/channels/channel_packet_dispatcher.cpp:248`; completion `R/channels/channel_runtime.cpp:1822`. 이 실험은 해당 codec/writer를 직접 부르며 L5 dispatch·L6 activation·L7 operation registry는 복원하지 않는다.

정상 메시지당 binding과 공통인 payload 생성·protobuf decode/encode·metrics는 그대로다. L1만 queue permit 예약/반환이 추가된다. L2만 요청 header 생성·파싱, reply header 생성·파싱이 추가된다. L2 target은 header/payload bytes를 복사하지 않고 header를 빌린다. L2 source·reply의 `message_parts_t` 2-element vector는 production envelope encoder 소유다. L2 source completion은 stack span 2개를 쓰며 service framing용 vector를 만들지 않는다. 추가 wait·metrics atomic·typed serializer 초기화는 없다. Core/binding 내부 allocation·atomic 총계는 추정하지 않는다.

Java `bf83275257`·.NET `2590b2a030`는 source envelope encode/target parse를 복원하고 raw reply를 유지한다. C++ L2는 네 경로의 envelope 기능을 모두 복원하므로 response encode와 completion parse도 포함한다. 이는 L2 기능 차이이며 L3·L4 비용은 포함하지 않는다. 언어 간 단계 손실을 직접 비교할 때 이 범위를 구분해야 한다.

판정 소유: envelope 형식·검증은 기존 codec 1곳, reply header는 기존 writer 1곳(전/후 각 1→1). 별도 정책 상태·queue·lock 없음. 이번 실험의 중복 parse 2→1(수신·응답 합계); protobuf 수동 completion shortcut 1→0. 복원 범위 밖 차이 0을 커밋 조건으로 삼는다.
