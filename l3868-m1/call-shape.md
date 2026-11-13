# #4350/#4351 C++ 호출 경로 대조

대조한 소스 HEAD는 `608513f260`이다. 이 HEAD의 L0/L1 runtime switch와 아래 호출 위치를 직접 확인했다. 측정 도구만 별도 로컬 커밋하며 runtime 소스는 바꾸지 않는다. 경로는 `/root/worktree/zlink-v21-s033/l3868/` 기준이다.

| 단계 | binding 호출 (file:line) | L0 호출 (file:line) | 같음? | L1 차이·복원 기능 |
|---|---|---|---|---|
| source payload 생성·stamp | `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:455` submit_raw → make_payload | `framework/bench/grpc/cpp/ladder/ladder_driver.inc:115` → 같은 submit_raw | 같음 | 없음; L1도 공통 submit_raw 사용 |
| source envelope·body part 생성 | `bench_cpp_client.cpp:459` 새 envelope, protobuf encode, 새 body | 같은 `bench_cpp_client.cpp:459` | 같음 | 없음 |
| source send·request submit | `bench_cpp_client.cpp:466` binding send/request async | 같은 `bench_cpp_client.cpp:466` | 같음 | 없음 |
| source completion wait | `bench_cpp_client.cpp:622` raw_driver completion thread POLLCOMPLETION | `ladder_driver.inc:38` → `raw_mesh_node_owner.cpp:524` POLLCOMPLETION | 같은 wait | 없음; 실제 owner socket/poller 사용 |
| target receive 전 | permit 없음 | permit 없음 | 같음 | L1만 실제 `application_job_queue_t::wait_for_supply_blocking(remote)`; `application_job_queue.hpp:568`, 호출 `raw_mesh_node_owner.cpp:535` |
| target receive | `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:41,84` stop.load → recv(none), 실패 시 50us sleep | `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:540` 같은 blocking recv·stop·오류 처리 | 같음 | permit을 확보한 뒤 같은 recv; 추가 readiness wait·drain 없음 |
| target handler 진입 | `bench_zlink_cpp_server.cpp:54,97` protobuf parse → metrics.record | `framework/bench/grpc/cpp/ladder/ladder_wire.hpp:210` fixed_handler.handle → protobuf parse → 같은 metrics.record | L0 허용 고정 handler 직접 호출 | L1만 `ladder_wire.hpp:322`에서 첫 handler instruction 직전 실제 permit.release_for_handler_entry; queue/worker·DI 복원 없음 |
| target reply 생성 | `bench_zlink_cpp_server.cpp:58` 새 envelope·reply.body 복사·protobuf encode·새 body | `ladder_wire.hpp:243` 같은 새 envelope·reply.body 복사·protobuf encode·새 body | 같음 | 없음; request part 재사용 없음 |
| target reply submit | `bench_zlink_cpp_server.cpp:67` received.reply 2-part submit | `ladder_wire.hpp:252` received.reply 2-part submit | 같음 | 없음 |
| source reply completion | `bench_cpp_client.cpp:476` binding reply observer → protobuf parse → stream.finish; admitted observer → stream.admit | 같은 submit_raw observer | 같음 | 없음 |

L0 고정 handler의 기존 wire shape·legacy 검사는 기준 HEAD에 포함된 직접 호출 경로다. 이 실험은 그 경로를 유지한다. L1에서 달라지는 작업은 target 수신 전 permit 확보, handler 직전 반환, receive 실패 시 RAII 반환이다. 별도 permit 모사나 source codec 경로 변경은 허용하지 않는다.

| 정상 record당 비용 | binding / L0 | L1 추가분 |
|---|---|---|
| source 생성·copy·counter | 같은 submit_raw·stream_t·observe_binding; 추가 생성·copy·atomic 0 | 0 |
| target wait | 같은 recv(none) 1회; 별도 readiness wait 0 | queue capacity wait 1회(여유 시 즉시 예약) |
| target decode·counter | 같은 protobuf decode·metrics.record; stop.load 1회 | 0 |
| reply 생성·copy | 새 envelope/body Message, reply body copy, protobuf encode; 같은 2-part submit | 0 |
| permit 내부 정상 예약·반환 | 없음 | 새 heap allocation 0·payload copy 0; queue mutex 1회, credits load 2회·fetch_add 1회·fetch_sub 1회, shared/weak refcount 수명; 별도 metrics atomic 0 |

Permit 횟수는 capacity 여유·waiter 없음·pressure transition 없음인 경로의 코드 계수다(`application_job_queue.hpp:454,903,922,984`). 실제 queue의 pressure·waiter 처리까지 같은 소유자가 수행한다. Binding/Core 내부의 조건부 allocation·atomic 총계는 추정하지 않으며 양쪽 공통 구현을 사용한다.
