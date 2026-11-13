# C++ L3/L4 profile 호출 대조

기준 scratch는 62e766f88d의 git archive다. Production typed JSON 파일은 df42a4df40과 동일함을 hash로 확인한다. 기존 L4 호출 대조: ../l3868-impl3/call-shape-L4.md. 아래 B=framework/bench/grpc/cpp, I=framework/languages/cpp/framework/include/zlink/framework, R=framework/languages/cpp/framework/src/runtime.

| 경로 | binding | L3 → L4 | 소유 기능 |
|---|---|---|---|
| source send | B/client/bench_cpp_client.cpp:455-472,651-654 submit_raw → fresh envelope/body/protobuf → binding send/request | B/ladder/ladder_wire.hpp:132-160, ladder_driver.inc:121-147; protobuf → serializer_registry | L4 typed JSON |
| target receive | B/zlink/bench_zlink_cpp_server.cpp:41,54-56 | R/mesh/raw_mesh_node_owner.cpp:530-546 blocking recv; B/ladder/ladder_wire.hpp:253-269 deserialize_typed_payload | L1 permit/L2 header/L3 framing/L4 decode |
| target reply | B/zlink/bench_zlink_cpp_server.cpp:58-67 fresh envelope/body/submit | B/ladder/ladder_wire.hpp:278-315 fresh body copy, serialize_handler_result, two-part submit | L2/L3/L4 |
| source completion | B/client/bench_cpp_client.cpp:623-630 pollcompletion → :472-482 protobuf decode / :466-469 admitted completion | B/ladder/ladder_driver.inc:132-138; ladder_wire.hpp:324-348 default typed decode + finish_bytes | L2/L3/L4 |

모든 단계는 recv 대기 1, drain 0, 같은 metrics counter를 유지한다. L4의 codec 내부 allocation/copy는 raw 계측에서 따로 센다. 추가 wait/queue/worker/metric atomic 0. 공통 규칙의 throughput 측정 조건 대신 이번 지시의 profile 전용 조건을 적용한다: measure.lock 없음, 모든 프로세스 CPUs 6-7,14-15, run ≤20초, throughput 판정 없음.
