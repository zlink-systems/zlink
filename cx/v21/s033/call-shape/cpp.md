# C++ call-shape: L0

비교 범위: binding benchmark의 `zlink-cpp` request/send 경로와 `zlink-framework-cpp` L0 ladder 경로. L0 측 source는 ladder 전용 변환 없이 공통 `submit_raw`를 호출한다. 대상은 `direct_server(..., raw_mesh=false)`가 `raw_mesh_node_owner_t::ablation_receive`로 수신하고 고정 handler를 직접 호출하는 경로다.

| path step | binding call (file:line) | L-step call (file:line) | same? | if different: restored feature |
|---|---|---|---|---|
| Source send — request/command: legacy envelope + protobuf payload 두 부분, async submit | `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:455-485` | `framework/bench/grpc/cpp/ladder/ladder_driver.inc:112-117` → 공통 `submit_raw` `bench_cpp_client.cpp:455-485` | yes | — |
| Target wait/receive — `recv(..., none)` 단일 수신 반복; poll batch drain 없음 | `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:37-45,80-87` | `framework/bench/grpc/cpp/ladder/ladder_wire.hpp:299-315`; 구현 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:526-537` | yes | — |
| Target per-message metrics — payload body를 metrics에 기록 | `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:46-56,89-100` | `framework/bench/grpc/cpp/ladder/ladder_wire.hpp:209-232` | yes | — |
| Target payload decode — 마지막 부분을 `BenchPayload.ParseFromArray` | `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:52-56,95-100` | `framework/bench/grpc/cpp/ladder/ladder_wire.hpp:211-231` | yes | — |
| Target reply build/submit — 새 response envelope와 protobuf를 만들고 2-part reply submit | `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:58-69` | `framework/bench/grpc/cpp/ladder/ladder_wire.hpp:238-252` | yes | — |
| Source completion/reply receive — binding `admitted`/`reply` completion callback | `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:465-484` | `framework/bench/grpc/cpp/ladder/ladder_driver.inc:112-165` (`L0`은 114-116의 공통 raw submit) | yes | — |
| Source per-message metrics — stream counters의 `admit`/`finish`, 잠금 갱신과 aggregate merge | `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:202-246,425-443,467-484` | 같은 공통 driver `stream_t`와 callbacks `bench_cpp_client.cpp:202-246,425-443`; L0 callback `ladder_driver.inc:112-117` → `bench_cpp_client.cpp:455-485` | yes | — |

## 실행 경로 주의

L0 실행의 `--ladder-raw-mesh` 기본값은 `0`이다 (`bench_cpp_client.cpp:1091`, `bench_framework_cpp_server.cpp:55`). 따라서 아래 별도 직접 socket 경로는 이번 L0 표의 대상이 아니며 **L0에서 사용하지 않음**: `ladder_wire.hpp:274-297`의 `if (raw_mesh)` 분기. 그 분기는 `recv(..., dontwait)` drain을 사용한다. 실제 L0은 `direct_server`의 `raw_mesh=false` 분기(`ladder_wire.hpp:299-315`)이며 `ablation_receive` 내부에서 `recv(..., none)`을 호출한다(`raw_mesh_node_owner.cpp:526-537`).
