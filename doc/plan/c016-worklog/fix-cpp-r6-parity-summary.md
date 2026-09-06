# C++ R6 parity 수정 결과

대상은 F-R6-6 → F-R6-5 → F-R6-8이며 D-105와 감독의 작업 지시에 따라 기존 결함(B)을 수정했다. `main`에서 작업했고 commit은 만들지 않았다. 이 작업의 변경 범위는 C++ 파일 13개와 이 보고서다.

## F-R6-6 — Create의 lifecycle 종료 처리

- **Spec:** [Actor 모델 §8.1·§9](../../../framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md), [MeshNode §7.1](../../../framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md). Logical intent 제거와 admitted peer 부재가 함께 성립하면 pending operation을 즉시 `Unavailable`로 끝낸다.
- **원인:** 수정 전 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:193`의 retry state는 `route_unavailable`을 deadline까지 반복했고, `:983`의 `forget_peer`와 연결되지 않았다.
- **소유 계층:** Framework mesh의 기존 expectation·topology가 lifecycle 사실을 결정하고 기존 operation registry가 pending terminal을 한 번만 전달한다.
- **수정:** `raw_mesh_node_owner.cpp:996`에서 기존 intent 제거 전이를 소비한다. `operation_registry.cpp:318`은 해당 target의 durable pending만 종료한다. Pending의 target 식별자는 기존 registry entry에 보관하며 별도 pending map, lifecycle flag, monitor, timer는 만들지 않았다. Registry에서 끝난 operation은 retry callback에서도 제출하지 않는다. 기존 durable bind도 같은 소유자를 사용한다.
- **교차언어 대조:** .NET `ZLinkManagedMeshNode.cs:460`의 expectation 제거 + admitted peer 부재 판정과 `:4304`의 durable pending 통지를 대조했다. C++에서는 해당 전이의 연결이 빠져 있었다.
- **변경 분류:** B — 기존 결함.
- **규칙 수:** 수정 전 mesh lifecycle 종료와 sender의 route 실패 종료가 분리된 2개 정책 → 기존 mesh 판정을 sender가 소비하는 1개 정책. 별도 종료 상태를 복제하는 대안은 채택하지 않았다.
- **회귀 테스트:** `test_cpp_framework_m6a_runtime.cpp:371`, CTest `test_cpp_framework_r6_create_lifecycle`. 미연결 target의 intent 제거, expectation을 먼저 제거하는 순서, 연결을 먼저 제거하는 순서를 검증한다. Peer가 남거나 expectation만 남은 중간 상태에서는 pending을 유지하고 최종 제거 후 deadline 전에 한 번만 `route_unavailable`로 완료한다.
- **Diff 분할:** `F-R6-6.patch` — `CMakeLists.txt`, `operation_registry.{hpp,cpp}`, `raw_mesh_node_owner.{hpp,cpp}`, `test_cpp_framework_m6a_runtime.cpp`의 lifecycle 관련 변경.
- **Gate:** focused lifecycle·registry 통과, 최종 unit gate 통과, 신규 CTest 5/5 통과.
- **BLOCKERS:** 없음.

## F-R6-5 — Remote Actor Join의 durable sender 통합

- **Spec:** [Actor 모델 §8.1·§9](../../../framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md). 동일 operation identity와 encoded request를 유지하고 각 attempt가 남은 deadline을 사용한다. Typed transient transport 실패만 replay하며 terminal envelope와 non-replayable 실패는 종료한다.
- **원인:** 수정 전 `raw_mesh_node_owner.cpp:2135`의 `port->request` 한 번과 `mesh_node_runtime.cpp:2822`의 분류가 Join의 결과를 결정했다. 상위 runtime은 timeout도 peer 부재를 근거로 `Unavailable`로 다시 분류했다.
- **소유 계층:** Framework의 기존 durable sender가 replay·admission 이력·deadline 종료 종류를 소유한다. Binding의 typed 결과를 소비하며 Core 연결 교체나 completion drain을 재구현하지 않는다.
- **수정:** `raw_mesh_node_owner.cpp:2090`의 Join을 create·bind와 같은 sender에 연결했다. Correlation과 wire 내용은 한 번 고정한다. Typed request failure가 남긴 admission 이력을 보존하여 미admission 소진은 `Unavailable`, admission 후 reply 유실 소진은 `DeadlineExceeded`로 끝낸다. Registry의 별도 `deadline + 75ms` 만료는 제거했고 실제 operation deadline·기존 retry 간격은 늘리지 않았다. Protocol error는 별도 terminal로 전달하며 `mesh_node_runtime.cpp:2837`은 sender의 최종 종류를 그대로 사용한다. 기존 completion payload의 pack/unpack은 `service_wire_codec.{hpp,cpp}`로 옮겨 Join과 기존 소비자가 함께 사용한다.
- **교차언어 대조:** .NET `ZLinkDurableRequest.cs`와 `ZLinkManagedMeshNode.cs:4304`, Java `ZLinkJavaDurableRequest.java`의 동일 wire·remaining deadline·typed admission 이력 처리를 대조했다. C++의 단발 Join 경로를 공통 sender로 정렬했다.
- **변경 분류:** B — 기존 결함.
- **규칙 수:** 수정 전 create/bind와 Join의 종료 정책 2개 → 공통 durable sender 1개. 상위 Join wrapper에 replay를 추가하는 대안은 채택하지 않았다.
- **회귀 테스트:** `test_cpp_framework_m6a_runtime.cpp:2609`, `:2688`, CTest `test_cpp_framework_r6_join_durable`. Route 생성 후 완료, reciprocal handover 후 동일 wire·correlation replay, application reply 보존, 미admission deadline, lifecycle 제거를 검증한다. Admission 후 상대 socket의 물리 단절에서도 원래 200ms deadline까지 유지하고 `DeadlineExceeded`로 끝난다. 기존 rejection·source fence·correlation mismatch 테스트도 포함한다. 로컬 `disconnect_rid`의 non-replayable ENOENT는 기존 raw port contract와 동일하게 유지한다.
- **Diff 분할:** `F-R6-5.patch` — F-R6-6 위에 적용한다. 공통 sender와 Join, registry의 protocol terminal, 기존 payload pack/unpack 이동, 상위 재분류 제거, Join 테스트·CTest 등록을 포함한다.
- **Gate:** focused Join 통과, M6B suite 통과, 최종 unit gate 통과, 신규 CTest 5/5 통과.
- **BLOCKERS:** 없음.

## F-R6-8 — Creation terminal의 원자적 공개

- **Spec:** [Spot·Actor membership §2](../../../framework/doc/framework/common/spec/server/03-spot-actor/05-spot-actor-membership.ko.md). Created/Rejected/Failed terminal을 authority·capacity 전환과 함께 공개하고 source RID + lifecycle generation + OperationId로 식별한다. Original deadline 뒤 5분까지 보존한다.
- **원인:** 수정 전 `provider_location_repository.hpp:713`에서 commit/abort를 완료한 뒤 `:741`에서 terminal만 별도로 썼다. 그 사이 provider가 실패하면 Ready authority에 대응하는 terminal이 없을 수 있었다.
- **소유 계층:** Framework provider location repository가 reservation fence·authority·capacity·terminal의 조건부 transaction을 구성하며 Store provider가 한 write의 원자성을 보장한다.
- **수정:** `provider_location_repository.hpp:678`의 completion이 기존 private commit/abort 경로에 terminal과 monotonic retention deadline을 전달한다. `:2003`의 write가 terminal의 Missing 조건과 Put을 같은 authority/reservation/capacity batch에 포함한다. 충돌·이미 완료된 reservation은 retained terminal을 다시 읽어 판정하며 Ready만으로 terminal을 만들지 않는다. Recovery의 공개 abort는 terminal을 만들지 않는다. 외부 deadline은 provider timestamp와 한 번 비교하여 steady-clock deadline으로 변환하며 보존 기간을 새 call에서 연장하지 않는다.
- **교차언어 대조:** .NET `ZLinkProviderLocationRepository.Authority.cs:833–924`의 단일 conditional write와 retained-terminal 충돌 판정을 대조했다. C++의 두 번째 write만 같은 전환으로 합쳤다.
- **변경 분류:** B — 기존 결함.
- **규칙 수:** 수정 전 authority/capacity 전환과 terminal 공개의 write 규칙 2개 → 조건부 publication 1개. Completion에 commit/abort 로직을 복사하는 대안 대신 기존 전환 코드를 재사용했다.
- **회귀 테스트:** `test_cpp_framework_opaque_store_providers.cpp:220`, `:228`, `:236`, CTest `test_cpp_framework_r6_creation_terminal`. Created/Rejected/Failed 각각에 대해 두 former write 사이의 실패, atomic write 응답 유실, conditional conflict를 주입하는 9개 GTest가 있다. 단일 batch의 authority·capacity·terminal 및 terminal Missing 조건, 재개한 repository의 stored terminal replay, 원래 expiry, 식별자 격리를 검증한다.
- **Diff 분할:** `F-R6-8.patch` — `provider_location_repository.hpp`, `test_cpp_framework_opaque_store_providers.cpp`, 해당 CTest 등록만 포함한다.
- **Gate:** 신규 9개 GTest 및 provider suite 통과, 최종 unit gate 통과, 신규 CTest 5/5 통과.
- **BLOCKERS:** 없음.

## 빌드·Gate 결과

| 검증 | 결과 |
|---|---|
| `cmake --preset linux-ninja-debug`, `cmake --build --preset linux-ninja-debug -j 3` | 통과. 마지막 fixture 변경 후 incremental preset build도 통과 |
| Focused lifecycle·Join·creation terminal | 통과 |
| M6B·opaque provider·operation registry subsystem | 3/3 통과, 51.85초 |
| 최종 `framework-unit` gate | **46/46 통과**, 97.02초 |
| 신규 CTest 3개 `--repeat until-fail:5` | **각 5/5 통과**, 총 15회, 4.82초 |
| `git diff --check` | 통과 |
| 원인별 patch 순차 적용 | 현재 C++ 변경 파일 13개와 byte 단위 일치 |

사용한 최종 gate 명령은 다음과 같다.

```bash
ZLINK_CPP_MESH_TRACE=1 flock -w7200 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build/linux-ninja-debug -L framework-unit --output-on-failure
ZLINK_CPP_MESH_TRACE=1 flock -w7200 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build/linux-ninja-debug -R '^test_cpp_framework_r6_' --repeat until-fail:5 -V
```

현재 preset의 무필터 CTest에는 74개가 등록되며 sample runner도 포함된다. 작업 지시의 **43개 baseline은 `framework-unit` label과 일치**하므로 sample 실행 금지에 맞춰 신규 3개를 포함한 46개를 gate로 실행했다. 무필터 74개 전체 gate와 sample runner는 실행하지 않았다.

로그와 분리 patch는 `/tmp/zlink-cpp-r6-parity/`에 있다. `gate.log`·`gate.trace.log`, `repeat5.log`, `test-subsystems.log`, `build-final.log`·`build-final-incremental-v2.log`를 보존했다. 원인별 patch는 **F-R6-6.patch → F-R6-5.patch → F-R6-8.patch** 순서로 적용하며 이 보고서는 patch에 포함하지 않는다.

## BLOCKERS·남은 실패

최종 unit gate와 신규 반복 테스트에 남은 실패는 없다. 무필터 CTest 전체는 위의 sample 금지 조건 때문에 미실행이다. 지정된 `framework/languages/cpp/AGENTS.md`는 저장소에 없어 루트·Framework 규칙과 실제 CMake preset을 적용했다. 보호된 문서, Core·binding, 다른 언어 트리는 이 작업에서 수정하지 않았다.
