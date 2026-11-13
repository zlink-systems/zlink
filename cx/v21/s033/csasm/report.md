# C++ send source 단일 조립 분리 보고서 (#3868)

## 파일과 hunk 판정

(A)는 Node record 독립 실행 변경으로 제외했다. (B)는 source가 최종 service payload buffer를 한 번 조립하도록 하는 변경으로 포함했다.

| 파일 | 판정 | 포함·제외 근거 |
|---|---|---|
| `framework/languages/cpp/CMakeLists.txt` | (A) 제외 | 새 independent node records test 등록만 포함한다. |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp` | (B) 포함 | Node send serializer가 payload와 일반 envelope type plan을 함께 넘긴다. |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/codecs/serializer.hpp` | (B) 포함 | request 전용 type plan을 send에도 쓸 envelope plan으로 일반화한다. |
| `framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp` | (B) 포함 | actor-bound command의 기존 deferred payload encoder 경로를 사용한다. |
| `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp` | (B) 포함 | channel encoder plan과 final service payload encoder를 send에 연결한다. |
| `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.hpp` | (B) 포함 | transport entry의 encoder 타입을 일반 envelope encoder로 바꾼다. |
| `framework/languages/cpp/framework/src/runtime/channels/channel_runtime_manager.cpp` | (B) 포함 | route channel fragment를 일반 envelope plan으로 만든다. |
| `framework/languages/cpp/framework/src/runtime/dispatch/application_record.hpp` | (A) 제외 | record claim storage 변경이다. |
| `framework/languages/cpp/framework/src/runtime/dispatch/offload_executor.cpp` | (A) 제외 | owner drain scheduling 변경이다. |
| `framework/languages/cpp/framework/src/runtime/dispatch/offload_executor.hpp` | (A) 제외 | owner drain scheduling API 변경이다. |
| `framework/languages/cpp/framework/src/runtime/host/app.cpp` | (B) 포함 | remote Node send가 native parts vector로 풀지 않고 message parts를 이전한다. |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp` | (A) 제외 | target Node record dispatch 변경이며 payload encoder와 독립이다. |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp` | (B) 포함 | `message_parts_t` send overload가 기존 target 분류와 deferred payload를 연결한다. 공통 operation helper와 request caller 이름 변경은 send와 request가 같은 분류 경로를 공유하기 위해 포함했다. Node record 실행 hunk는 없다. |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.hpp` | (B) 포함 | `message_parts_t` overload 및 shared operation helper 선언이다. |
| `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp` | (A) 제외 | FIFO claim과 record ownership 변경이며 (B)에 필요하지 않다. |
| `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.hpp` | (A) 제외 | FIFO claim 자료구조 변경이며 (B)에 필요하지 않다. |
| `framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp` | (B) 포함 | codec이 header를 받아 final payload encoder로 전달한다. |
| `framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.hpp` | (B) 포함 | deferred payload encoder callback과 type plan을 일반화한다. |
| `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp` | (B) 포함 | header kind를 실제 envelope header에서 쓰고 service payload writer를 공유한다. |
| `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.hpp` | (B) 포함 | 일반 envelope type plan을 선언한다. |
| `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp` | 혼합, (B) hunk만 포함 | `send_to_node(message_parts_t)` overload는 포함했다. claim transfer hunk는 (A)이므로 제외했다. |
| `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.hpp` | (B) 포함 | message parts send overload 선언이다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_claim_dispatch_cost.cpp` | (A) 제외 | claim batch 비용 변경 테스트다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_envelope_allocations.cpp` | (B) 포함 | Node send의 final envelope allocation을 확인하도록 갱신한다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_execution.cpp` | (A) 제외 | 독립 실행 및 gate 음성 대조 변경이다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp` | (A) 제외 | owner claim batch 변경이다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_mailbox_fifo_cost.cpp` | (A) 제외 | mailbox FIFO 비용 변경이다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_mailbox_owner_dispatch.cpp` | (A) 제외 | owner dispatch 변경이다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_maintenance_failure.cpp` | (A) 제외 | claim allocation 실패 정리 변경이다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_node_vertical.cpp` | (A) 제외 | 같은 Node successor 동시 진입 assertion 변경이다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_relocation_seal_contract.cpp` | (A) 제외 | claim settlement 변경이다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_wire_identity.cpp` | (B) 포함 | command kind 및 wire byte identity 검증을 유지한다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_route_client_channel_routing.cpp` | (B) 포함 | route channel 준비 helper의 이름 변경을 반영한다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_route_payload_ownership.cpp` | (B) 포함 | deferred service payload를 포함한 Node send allocation 경계를 검증한다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_send_allocations.cpp` | (B) 포함 | Node send가 encoding 외 결과 storage를 추가하지 않는지 검증한다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_independent_node_records.cpp` | (A) 제외 | 새 독립 claim 검증 테스트다. |

`service_mailbox`, `offload_executor`, `mesh_node_host_service`의 hunk는 (B)에 필요하지 않아 모두 제외했다. (B)는 이미 존재하는 `application_envelope_encoder_t`와 `take_application_payload()` 소유권 경로만 사용한다.

## 검증

전체 C++ unit/contract CTest에서 306개 중 300개가 통과했고 6개가 timeout으로 실패했다. `typed_json_compile_fail_0`부터 `_4`까지는 각각 고정 30초 timeout, `test_unreal_stream_connector_package_contract`는 120초 timeout이었다. 해당 실패는 통과로 간주하지 않았다. 로그는 `csasm/logs/ctest-unit-contract.log`에 있다. 전체 대상 빌드는 583/583 단계가 성공했고 로그는 `csasm/logs/build-unit-contract.log`에 있다. B 전용 벤치 대상 빌드는 main과 변경 모두 성공했다. 로그는 `csasm/logs/build-bench-main.log`, `csasm/logs/build-bench-change.log`에 있다. 구성 로그는 `csasm/logs/configure-retry.log`, `csasm/logs/configure-bench-main.log`, `csasm/logs/configure-bench-change.log`에 있다.

B 관련 CTest 5개(`route_client_channel_routing`, `envelope_allocations`, `request_wire_identity`, `send_allocations`, `route_payload_ownership`)는 모두 통과했다. `git diff --check`와 `scripts/format/format.sh --check cpp`도 통과했다. 형식 검사는 `csasm/logs/format-check-final.log`에 있다.

## 측정

main은 `f784975639a2779a81d829dd3807aa9dded19b0f`, 변경은 (B)만 적용한 빌드다. source CPU 0–7, target CPU 8–15로 고정하고 같은 실행 세션에서 binding 기준을 측정했다. 네 workload는 각 5회씩 순서를 교차했고, send-saturation 1 KiB는 common rate 136,889 msg/s로 main과 변경을 각 3회 측정했다. 각 셀은 `check_results.py` 검사를 통과했다. 총 69개 셀이 유효했고, 음성 대조 10개 입력은 모두 거부됐다. 전체 원시 결과는 `csasm/runs5/`, `csasm/paced/`, 판정 결과는 `csasm/quality.json`, 요약은 `csasm/summary.json`, batch 로그는 `csasm/logs/measure-batch.log`에 있다.

| workload | 구현 | F÷B | Framework µs/message | budget µs/message | over-budget µs/message |
|---|---|---:|---:|---:|---:|
| send-saturation 1 KiB | main | 0.293 | 5.585 | 2.725 | 2.861 |
| send-saturation 1 KiB | (B) | 0.286 | 5.708 | 2.725 | 2.983 |
| send-saturation 4 KiB | main | 0.535 | 6.318 | 5.629 | 0.689 |
| send-saturation 4 KiB | (B) | 0.525 | 6.434 | 5.629 | 0.805 |
| request-backpressure 1 KiB | main | 0.209 | 19.271 | 6.718 | 12.553 |
| request-backpressure 1 KiB | (B) | 0.206 | 19.611 | 6.718 | 12.893 |
| request-serial 1 KiB | main | 0.569 | 120.156 | 113.954 | 6.202 |
| request-serial 1 KiB | (B) | 0.528 | 129.391 | 113.954 | 15.437 |

F÷B는 각 구현의 Framework throughput 중앙값을 같은 workload의 binding throughput 중앙값으로 나눈 값이다. budget은 binding µs/message ÷ 0.6이고 over-budget은 Framework µs/message에서 budget을 뺀 값이다. 네 workload 모두 (B)의 처리량이 main보다 낮았고 request-serial에서 차이가 가장 컸다.

같은-rate send-saturation 1 KiB에서 source+target user+kernel CPU를 합산한 중앙값은 main 22.446 µs/message, (B) 23.806 µs/message였다. source는 9.071에서 9.863, target은 13.375에서 13.943 µs/message로 증가했다. 목표 수신률 136,889 msg/s를 세 번 모두 ±1% 안에서 맞췄다.

## 판정 위치와 중복 작업 점검

(B)의 final send buffer allocation 판정 위치는 기존 2곳에서 1곳으로 줄었다. 이전 `envelope_codec.cpp:513` header encoding은 header 전용 `message_t`를 할당했고, `protocol/service_wire_codec.cpp:3224`의 service payload encoding은 final message를 별도로 할당해 header bytes를 복사했다. 새 `envelope_codec.cpp:528`는 header 크기만 측정하고 writer를 final service payload encoder에 넘겨, 최종 message 한 개에 직접 쓴다. 크기 측정용 header writer pass는 남지만 header byte buffer 복사는 제거됐다. 이전 `app.cpp:2617`의 remote Node 경로는 `message_parts_t::take_items()`로 native vector로 변환한 뒤 Node send에 넘겼다. 현재 `app.cpp:2617`은 parts 소유권을 직접 전달한다. 같은 함수의 local 경로 `app.cpp:2613`은 별도 local submit API 계약 때문에 native vector 변환을 유지한다.

기타 관찰한 send path 변환은 `channel_runtime.cpp`의 type/channel preparation과 `envelope_codec.cpp:528`의 header size/write pass다. 이들은 최종 wire header 구성에 필요해 남았다. 이번 변경으로 `service_wire_codec.cpp` 내부 header 및 body 조립을 따로 바꾸지는 않았다.
