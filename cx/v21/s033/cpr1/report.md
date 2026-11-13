승인된 대안 B의 PR 1을 구현했다. 기본 typed node request는 기존 final receiver의 admission, public reply, physical reply와 C1 완료 슬롯을 사용한다. Bool admission task, submit_result 변환 task와 mesh admission observer를 제거하고, binding의 result/admitted/reply를 최종 admission view에 직접 연결했다. 최종 typed 전달과 C1 terminal의 판정자는 바꾸지 않았다.

Task facade의 결과 handle은 intrusive 소유자로 통일했다. Admission, public reply와 physical reply는 같은 소유자의 view이며, 결과 준비 여부와 observer 등록은 같은 mutex와 condition variable을 사용한다. 첫 observer와 binding observer coroutine frame은 그 소유자의 arena를 사용한다. 여러 observer는 같은 arena의 확장 저장소를 사용하며 개수 제한을 두지 않았다. Codec view 생성과 observer 등록이 동시에 할당할 수 있어 arena 할당만 별도로 직렬화한다. 기존 세 stage의 mutex 세 개를 결과 mutex 하나와 allocator mutex 하나로 대체했다. Allocator mutex는 결과나 실행 권한을 판정하지 않는다.

Public 종료와 physical 종료는 별도 사실로 유지했다. Native observer frame은 parameter 소멸과 frame 반환이 끝날 때까지 request 소유자를 보유한다. Public task가 먼저 끝나거나 파괴돼도 physical observer가 끝나기 전 저장소를 반환하지 않는다. Callback과 projection은 기존 physical 처리 종료 시 정리하며, 보관된 public 결과가 callback을 계속 보유하지 않게 했다. Host/port와 binding async_result의 수명 참조는 유지했다.

준비된 request 저장소는 process 수명의 재사용 allocator를 사용한다. 일반 task의 inline arena는 512바이트, prepared receiver는 4096바이트다. 확장 할당은 같은 소유자가 처리하며 observer 개수를 제한하지 않는다. 일반 ready task는 같은 intrusive handle과 같은 할당 본문을 사용하되 기존 단일 할당을 유지한다. 기존 내부 shared completion source factory의 표준 control block도 같은 소유자의 arena에 배치해 단일 할당을 유지했다. 이전 shared-state 요청 경로와 병행하는 별도 task 구현이나 새 public API는 없다. Encoding plan, wire, C1 map node allocator, socket 직렬화는 PR 2·3의 범위이므로 수정하지 않았다.

호출 thread 예산은 request-backpressure 1 KiB의 12.10 µs/request다. 벤치마크는 실행하지 않았으며 이 예산의 달성 여부는 감독자의 측정으로 판정해야 한다.

최종 검증은 모두 통과했다. 지정된 공용 락과 build slot을 사용해 linux-ninja-release의 전체 타깃을 --parallel 3 -- -k 0으로 빌드했다(build-final.log). ctest --preset linux-ninja-release -L '^(unit|contract)$'는 298/298개가 통과했고 173.11초가 걸렸다(test-all.log). 저장소 루트의 scripts/format/format.sh --check cpp도 통과했다(format-final.log). 남은 실패는 없다. Maintenance snapshot의 할당 실패 처리와 local enqueue 예산은 기존 실패 주입 조건과 assertion을 바꾸지 않고 통과했다. #3919/#3957의 예외, timeout, cancel, shutdown, 늦은 physical reply와 한 번 전달 검사를 포함했다.

RED 검사는 변경 전 receiver/task header로 warmed 결과 저장소와 첫 pending observer의 heap 할당 2회를 검출했다(red-test.log). 일반 ready task의 첫 result() 참조에 pool을 적용한 중간 구현은 기존 1회 계약 대신 6회 할당해 실패했다(ready-red.log). 같은 검사에서 일반 task의 단일 할당을 복원한 뒤 1회를 확인했다(ready-green.log). Ready task의 이동, 참조 공유, void, mixed, allocation, concurrency를 포함한 7개 검사도 통과했다(test-ready.log).

최종 계측 출력(result-counts.log)에서 warmed 요청 32개의 caller heap 할당은 0, shared_ptr control-block 연산은 0이었다. 일반 shared_ptr 대조 입력은 연산 6회를 검출했다. Cold 준비 할당은 7회, worker 반환 중 할당은 0회, public shared source 생성은 1회였다. 128개 observer와 동시 codec view 할당도 통과했다. 계측은 빈 ambient scope의 기본 관찰 경로를 사용한다. 범위는 caller의 warmed receiver 생성, admission view, 첫 pending public observer 등록, ready 결과 읽기와 task handle 정리다. Libstdc++의 strong 증가·감소, weak 증가·감소와 weak lock 연산 5종을 계측하며 일반 shared_ptr 복사·해제를 대조 입력으로 검출한다. Binding bundle, native message, codec, C1 map node, host/transport 수명 참조와 dispatcher 실행은 이 경계 밖이다. Cold growth와 worker의 저장소 반환 중 할당은 별도로 출력하며 추가 observer의 확장 할당을 0으로 주장하지 않는다.

추가 contract 검사는 ready binding observer frame의 arena 할당 0회와 일반 heap frame 1회를 대조해 통과했다. Public timeout 후 늦은 physical reply를 한 번 처리하고, physical observer가 끝나기 전에 기존 주소를 재사용하지 않는지도 검사한다. 128개 observer와 codec view의 동시 arena 할당도 검사한다. 기존 #3919/#3957의 예외·수명·한 번 전달 검사는 유지했다. 재사용 allocator의 page를 request identity로 볼 수 없어, 기존 coallocation 검사는 heap chunk 주소 대신 실제 수명 소유자와 arena identity를 비교하도록 바꿨다. Terminal, 결과와 오류 assertion은 완화하지 않았다.

소유 계층은 Framework의 logical operation, deadline, typed delivery와 execution gate이며, physical admission과 native completion은 binding/Core가 소유한다.
스펙 조항은 01-submit-and-completion §9–11(331–333, 357–365, 392–428, 447–473, 490–519), 08-messaging-hot-path §4.4와 06-state-ownership-and-lanes §5다. 스펙은 수정하지 않았다.
교차언어 대조는 승인 보고서의 .NET TryRemoveOperation, Java/Kotlin operation registry take와 completion dispatcher, Node.js operation-registry take와 queueMicrotask의 공통 소유를 기준으로 삼았다. 사용자 동작의 변경이 없는 C++ 저장 표현 변경이며 다른 언어 코드는 수정하지 않았다.
변경 분류는 B다. 승인된 중간 결과 표현과 관찰 연결의 비용 결함을 수정했으며, 새 retry, timer, poller, pending 표나 admission cap을 만들지 않았다.

리팩토링 점검: 성능 6건, POSDDD 4건, 불필요 코드 4건 발견. 고친 항목은 admission task chain, 결과 control block, 기본 observer heap, binding observer frame, 일반 ready task의 pool 회귀, 일반 task의 arena 과대 할당, 결과 동기화 소유, arena 할당 경쟁, task state 복사 금지, allocator 수명, request 전용 shared allocator, stage별 shared alias override, submission observer와 제품에서 호출되지 않는 compose_node_request_reply다. 넘긴 항목은 없다.

수정 전/후 규칙 수는 C1 terminal 1→1, deadline 1→1, native admission 1→1, typed 전달 1→1이다. 중간 표현의 제거와 판정 위치 수를 구별했다. 판정 소유자는 다음과 같다.

| 결정 | 소유자와 판정 위치 | 수정 전 | 수정 후 |
|---|---|---:|---:|
| C1 terminal | Framework operation_registry_state_t::take, operation_registry.cpp:539 | 1 | 1 |
| Deadline 만료 | Framework operation owner의 deadline index | 1 | 1 |
| Native admission과 WRITABLE | Binding/Core의 submission result와 admitted | 1 | 1 |
| 최종 typed 결과 전달 | 기존 request receiver의 projection과 delivery | 1 | 1 |
| 사용자 continuation turn | 기존 process completion dispatcher와 execution gate | 1 | 1 |
| Physical submit 직렬화 | 기존 Framework socket mutex와 Core gate, PR 1 범위 밖 | 2 | 2 |

제거한 메커니즘은 다음과 같다.

- 기본 typed node request의 bool→submit_result→void 변환 task와 admission observer 연결을 최종 admission view로 대체했다.
- 결과 state/view와 기본 observer의 std::shared_ptr control block을 intrusive 소유자로 대체했다. Strong/weak 수명 counter는 기존 control block의 참조를 대체하며 terminal을 판정하지 않는다.
- Stage별 mutex와 condition variable을 공통 결과 동기화로 대체했다. Arena 할당 lock은 별도 allocator 사실만 보호한다.
- Pending observer와 내부 binding coroutine frame의 별도 heap 저장을 기존 request arena로 대체했다.
- Request capture 전용 shared allocator와 stage의 virtual shared_from_this/weak_from_this alias override를 제거했다.
- 제품에서 호출되지 않는 compose_node_request_reply와 compose_node_request_submission을 제거했다. 이전 synthetic host/admission 시나리오의 assertion은 test support에서 유지하며 실제 node 경로 검사는 raw/host bridge를 사용한다.

변경 파일은 framework/languages/cpp 기준으로 다음과 같다.

- CMakeLists.txt
- framework/include/zlink/framework/contracts/dispatch/request_task.hpp
- framework/include/zlink/framework/contracts/dispatch/task.hpp
- framework/include/zlink/framework/detail/task_owner.hpp
- framework/src/runtime/actors/actor_gateway_runtime.cpp
- framework/src/runtime/backend/raw_binding_adapter.hpp
- framework/src/runtime/backend/raw_dealer_port.cpp
- framework/src/runtime/backend/raw_dealer_port.hpp
- framework/src/runtime/backend/raw_route_port.cpp
- framework/src/runtime/backend/raw_route_port.hpp
- framework/src/runtime/channels/channel_runtime.cpp
- framework/src/runtime/foundation/operation_registry.cpp
- framework/src/runtime/foundation/operation_registry.hpp
- framework/src/runtime/mesh/mesh_node_runtime.cpp
- framework/src/runtime/mesh/mesh_node_runtime.hpp
- framework/src/runtime/mesh/raw_mesh_node_owner.cpp
- framework/src/runtime/mesh/raw_mesh_node_owner.hpp
- framework/src/runtime/spots/spot_runtime.cpp
- framework/src/runtime/stateful/public_host_runtime.cpp
- framework/src/runtime/stateful/public_host_runtime.hpp
- tests/Zlink.Framework.ContractTests/test_cpp_framework_contract_headers.cpp
- tests/Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp
- tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_late_result.cpp
- tests/Zlink.Framework.UnitTests/test_cpp_framework_prepared_result_storage.cpp
- tests/Zlink.Framework.UnitTests/test_cpp_framework_ready_task.cpp
- tests/Zlink.Framework.UnitTests/test_cpp_framework_request_call_allocations.cpp
- tests/Zlink.Framework.UnitTests/test_cpp_framework_request_native_counts.cpp
- tests/Zlink.Framework.UnitTests/test_cpp_framework_request_transport.cpp
- tests/Zlink.Framework.UnitTests/test_cpp_framework_request_transport_projection.cpp
- tests/Zlink.Framework.UnitTests/test_cpp_framework_route_payload_ownership.cpp
- tests/support/node_request_submission_fixture.hpp
