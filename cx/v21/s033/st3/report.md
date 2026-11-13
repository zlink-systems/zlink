# #1639 설계 단계 3 — C++ record dispatch

## 결과

등록 때 route별 Node/Channel dispatcher 쌍과 codec binding을 준비하고, record에서는 등록된 route plan을 조회해 typed completion으로 coroutine을 실행한다. `mesh_node_host_service_t::start`의 plan 구성은 [mesh_node_host_service.cpp:2662-2672](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2662), plan 생성과 조회는 [mesh_record_dispatcher.cpp:14-49,70-85](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:14)에 있다. Node terminal은 ordinary path에서 stack 소유로 전달되고, 기존 defer 지점에서만 승격한다 [mesh_node_host_service.cpp:2323-2345](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2323).

### 결정 소유 표

| 결정 | 변경 전 소유 위치 | 변경 후 소유 위치 |
|---|---|---|
| record dispatcher 생성 | receive 및 application dispatch의 두 호출부 [mesh_node_host_service.cpp:2342-2347,3825-3832@HEAD](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2342) | 등록 시 plan 생성 [mesh_node_host_service.cpp:2662-2672](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2662) |
| route와 Node/Channel 선택 | record마다 route dispatcher 생성·종류 선택 [mesh_record_dispatcher.cpp:49-60@HEAD](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:49) | 등록 시 route별 쌍 생성, record에서 route 조회 후 kind 선택 [mesh_record_dispatcher.cpp:31-48,70-85](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:31) |
| request reply 제출 | record별 캡처 lambda [mesh_record_dispatcher.cpp:61-67@HEAD](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:61) | typed reply token을 host에 직접 전달 [route_packet_dispatcher.cpp:452-459](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:452), 종료된 host는 `terminated` 반환 [public_host_runtime.cpp:9937-9942](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:9937) |
| handler completion | send별 `std::function` closure [route_packet_dispatcher.cpp:303-345@HEAD](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:303), erased variant [route_handler_invoker.hpp:27-31@HEAD](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.hpp:27) | send/request typed variant와 한 visitor [route_handler_invoker.hpp:17-44](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.hpp:17), coroutine completion [route_handler_invoker.cpp:65-93](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.cpp:65) |

Dispatcher 생성 판정 위치는 record별 호출부 2곳에서 registration 1곳으로 줄었다. route/kind 조회는 record당 map lookup 및 kind 선택 1곳으로 남겼다. 전체 규칙 수를 줄이는 구현으로, 판단 규칙 **2→1**이다.

### Per-record 객체와 할당 수

| 항목 | 변경 전 | 변경 후 |
|---|---:|---:|
| `mesh_record_dispatcher_t` 객체 | 1 / record | 0 / record (registration당 1) |
| `route_packet_dispatcher_t` 객체 | 1 / record | 0 / record (등록 route당 Node/Channel 각 1) |
| reply 제출 `std::function` closure | 1 / record | 0 |
| send completion `std::function` closure | 1 / send record | 0 |
| completion variant 내부 할당 | type-erased closure 저장소 1 / send | 0; send/request typed alternative 각각 계수기 검사 |
| ordinary Node terminal shared owner + settlement closure | 1 + 1 / record | 0 + 0; 실제 defer 분기에서만 기존 승격 유지 |

Allocation counter는 완성한 typed completion을 variant에 넣을 때 send/request 각각 0 allocation을 확인한다 [test_cpp_framework_channel_messaging.cpp:889-914](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_channel_messaging.cpp:889). 전체 runtime 할당은 coroutine frame, fresh scope, handler 및 scoped dependency 수명을 포함하므로 0으로 주장하지 않는다. Header는 수신 경로에서 decode한 값을 overload에 넘겨 한 번 사용하고 [mesh_node_host_service.cpp:2317-2345](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2317), 해당 overload는 재decode하지 않는다 [route_packet_dispatcher.cpp:166-200](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:166). Route handler는 kind 분기 뒤 한 번 조회한다 [route_packet_dispatcher.cpp:206-210,305-313](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:206).

## 계약과 분류

- 소유 계층: Framework route registration은 immutable route plan을 소유하고, 종료 뒤 reply 수락은 Core host runtime이 판정한다 [mesh_node_host_service.cpp:2662-2672](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2662), [public_host_runtime.cpp:9937-9942](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:9937).
- Spec: 새 scope·handler/filter instance와 scoped dependency는 dispatch마다 생성·정리한다 [06-framework-api.ko.md:485-489](/root/worktree/zlink-v21-s033/st3/framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:485). Node/Channel handler scheduling과 gate는 변경하지 않았다 [02-handler-turn-and-execution-gate.ko.md:97-105](/root/worktree/zlink-v21-s033/st3/framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:97).
- 교차언어: .NET도 invocation마다 scope 및 handler를 만들고 [ZLinkHandlerDispatcher.cs:96-102](/root/worktree/zlink-v21-s033/st3/framework/languages/dotnet/src/Zlink.Framework/Runtime/Handlers/ZLinkHandlerDispatcher.cs:96), Java도 invocation마다 activation과 handler를 연다 [ZLinkChannelHandlerInvoker.java:443-449](/root/worktree/zlink-v21-s033/st3/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelHandlerInvoker.java:443). C++ 변경 후 이 수명과 일치한다.
- 분류: **B — 기존 per-record allocation 결함 수정**. 설계 R6–R9 / #1639 단계 3에 따른다.
- 리팩터 점검: 변경 dispatch 경로에서 성능·POSDDD·불필요 코드 기준 추가 발견 0, 해결 못한 항목 0. Coroutine layer와 execution/scheduling 경로는 유지했다.

## 검증

- Release, tests ON, bindings source ON, 25 GB cap, `-j3 -k 0` 설정으로 C++ Framework 전체 빌드 완료. 마지막 변경은 flow trace 조건에 중괄호만 명시한 것이며, 그 뒤 `format.sh --check cpp`와 source scan gate를 통과했다. 공유 슬롯이 15분 넘게 점유되어 이 표기 수정 뒤 대상 재빌드는 시작되지 않았다.
- `test_cpp_framework_channel_messaging`: 50회 통과. 새 테스트는 typed completion 할당 0, record별 handler/scope/scoped dependency identity, constructor 예외 terminal 1회, 종료된 reply terminal 1회를 검사한다 [test_cpp_framework_channel_messaging.cpp:916-1018](/root/worktree/zlink-v21-s033/st3/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_channel_messaging.cpp:916).
- C++ Framework unit+contract suite 292개를 한 번 실행했다. 최초 290개 통과, `source_scan_gate`는 중괄호 수정 후 통과, `tooling_contract`는 현재 source build에서 설치한 binding prefix로 환경을 보완한 뒤 통과했다. 실패했던 두 검사는 각각 재실행해 통과했다.
- `bash scripts/format/format.sh --check cpp`: 통과.
