# C++ mesh shutdown seal·admission parity (D-097)

D-097(B)의 C++ 후속 구현이다. Host shutdown seal을 raw mesh가 조회해 Hello 시작을 막고,
host의 Draining publication을 local descriptor와 admitted peer의 Update에 연결한다.
반복 admission은 완료 진단을 남기면서 admission epoch·변경 알림·liveness를 다시 갱신하지 않는다.
Branch는 `main`이며 commit은 없다. 기존 Node binding 변경과 untracked 파일은 보존했다.
검증 종료 시 함께 나타난 Java runtime 변경은 다른 작업의 diff로 보고 건드리지 않았다.

## 진단과 선택

- 수정 전 `raw_mesh_node_owner.cpp:3895`의 READY edge는 seal 조회 없이 Hello를 보냈다.
  Seal 소유자는 `runtime/host/app.cpp:449`의 `app_state_t::draining`이다.
  `:3545`의 Shutdown만 이를 true로 설정하고, `:1375`에서 모든 mesh의 기존
  `spot_state->drain_flag`에 동일 shared atomic을 연결한다. Relocation의
  `mesh_node_host_service_t::seal_application_dispatch` 및 descriptor Draining은 이 atomic을 설정하지 않는다.
- 수정 전 `mesh_node_host_service.cpp:2490`의 Draining publication은 Location Store만 갱신했다.
  Raw mesh에는 Update 수신만 있고 송신은 없었다. Local Draining을 게시한 뒤 peer 손실을
  보상하는 방식 대신, 기존 host publication에서 raw descriptor·Update를 게시하도록 연결했다.
- 수정 전 `trace_admission_phase():1178`은 이미 duplicate Admit에도 `bilateral-ready`를 기록했다.
  .NET 보고서의 “duplicate 분기에서 완료 진단 누락”은 C++ Admit에 그대로 해당하지 않는다.
  다만 duplicate Hello의 phase는 누락됐고, 동일 descriptor·connection의 재입력은 topology에서
  `admitted`로 처리돼 epoch·변경 알림·liveness를 다시 갱신했다. 성공 tail을 통합하고 동일
  admission을 기존 `duplicate_connection` 결과로 분류한다.
- 대안: .NET처럼 callback setter와 wrapper를 추가하거나, 기존 shared atomic을 내부 raw options에
  전달할 수 있다. C++은 이미 모든 mesh의 Spot 상태에 host atomic이 있으므로 후자를 선택했다.
  새 seal flag·timer·poller·reconnect 정책은 없다.

## 변경 파일과 위치 (변경 후)

경로 기준은 `framework/languages/cpp/`다.

| 파일:행 | 변경 |
|---|---|
| `framework/src/runtime/mesh/raw_mesh_node_owner.hpp:69` | 내부 options에 host atomic의 const shared reference. `:187`은 Draining publication 진입점. |
| `framework/src/runtime/mesh/mesh_node_runtime.cpp:800` | 기존 Spot drain flag를 raw options에 전달. Host가 유일한 작성자다. |
| `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3905` | READY 처리는 유지하고 Hello 시작 직전에 host seal을 acquire로 조회. |
| `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:663` | Local Draining descriptor를 게시하고 admitted peer에 canonical Update 송신. |
| `framework/src/runtime/mesh/mesh_node_host_service.cpp:2493` | 기존 host Draining publication을 raw publication에 연결. Store가 없는 manual mesh도 적용. |
| `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1198` | accepted·duplicate 성공 결과에 동일 command별 phase 진단. 문자열 생성 전 trace gate 적용. |
| `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:2915` | duplicate 조기 반환·별도 Admit 응답 분기를 제거하고 성공 tail 공유. |
| `framework/src/runtime/mesh/service_topology_registry.cpp:284` | 동일 descriptor·connection의 반복 admission은 epoch·change handler를 갱신하지 않음. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_shutdown_seal.cpp:60` | sealed 재연결의 Hello 부재·Draining Update·peer 손실·종료, unsealed Serving/Draining 재admission. |
| 같은 파일 `:122`, `:149` | liveness expiry·실패한 Update 뒤 Draining 유지, 교차 Hello/Admit 양쪽 완료 로그·변경 알림 1회·epoch 불변. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_node_vertical.cpp:593`, `:1348` | host publication의 local 상태 및 기존 host seal의 실제 raw 배선 검증. |
| `CMakeLists.txt:1109` | 신규 focused test를 `framework-unit` label로 등록. |

## 네 줄

- 소유 계층: Framework host가 shutdown seal을 소유하고 raw mesh·topology가 logical admission, local descriptor와 완료 진단을 소유한다. 물리 reconnect·pipe 선택은 Core/binding 소유로 유지한다.
- Spec 조항: `05-host-relocation-flow.ko.md` §14 1단계, `03-mesh-node.ko.md` §6·§7.1, `06-wire-protocol.ko.md`의 반복 Hello/Admit idempotence, D-097.
- 교차언어 대조: .NET D-097 구현은 gate owner 조회와 네 peer 전이의 Draining 보존 helper를 사용한다. C++은 `public_host_runtime.cpp:1265`가 local descriptor에서 상태를 계산하며 peer 전이는 local descriptor를 쓰지 않는다. 따라서 상태 helper를 추가하지 않고 기존 단일 계산 규칙을 유지한다. C++에만 없던 raw Draining publication과 같은 connection의 idempotence를 보완했다.
- 변경 분류: B — D-097 승인 범위의 기존 결함. Public API·wire schema 변경 없음.

## 수정 전/후 규칙 수

- 신규 admission의 shutdown 정책: host는 seal을 따르고 mesh는 무조건 Hello를 보내는 2개 정책 → 기존 host seal을 공유하는 1개 정책.
- accepted/duplicate 응답 tail: 별도 2개 → 공통 1개. Hello 진단 정책: accepted 기록/duplicate 누락 2개 → 성공 결과 공통 1개.
- 동일 admission의 변경 알림: 교차 Hello/Admit당 2회 → 1회. Admission epoch·liveness 소유자는 각각 기존 registry 1개 그대로다.
- Peer 전이 뒤 local 상태 규칙: descriptor 기반 1개 → 1개. .NET식 별도 상태 보존 분기 0개 추가.
- Draining publication: Store만 Draining이고 raw는 Serving인 2개 동작 → host publication 하나에서 Store와 raw Update에 적용. 새 상태·timer 0개.

## 검증 결과

| 검증 | 결과 | 로그 |
|---|---|---|
| `cmake --build build -j4` | exit 0. 수정한 test fixture는 해당 target만 추가 빌드했다. | `/tmp/cpp-mesh-shutdown-build.log`, `/tmp/cpp-mesh-shutdown-test-build.log` |
| 신규 `test_cpp_framework_mesh_shutdown_seal` | passed, 0.92 s. 위 회귀 항목 모두 단언. | `/tmp/cpp-mesh-shutdown-focused.log` |
| 기존 `test_cpp_framework_m6a_runtime` | passed, 5.20 s | `/tmp/cpp-mesh-shutdown-m6a.log` |
| `zlink_cpp_framework_mesh_node_vertical_test` | passed, 1.28 s | `/tmp/cpp-mesh-shutdown-vertical.log` |
| `flock -w7200 /tmp/zlink-samples-gate.lock bash samples/run_samples.sh` ×1 | exit 0, **7/7**, Bingo client/server self-check, ZoneWorld B8·G4 child checks 및 `ZW-E5 verdict=PASS`, `sample all result=passed` | `/tmp/cpp-mesh-shutdown-samples.log` |
| `ctest --test-dir build -L framework-unit --output-on-failure` ×1 | exit 0, **42/42 passed**, 46.01 s | `/tmp/cpp-mesh-shutdown-unit.log` |
| `git diff --check` | passed | working tree |

Sample gate에는 기존 `ZLINK_CPP_MESH_TRACE=1`, `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`을
켜서 실행했다. 신규 unit은 C++의 기존 trace를 직접 capture해 양쪽 `bilateral-ready`와
`result=1`(`duplicate_connection`)을 단언한다. .NET 보고서에 있던 로그 단언 blocker는 C++에는 없다.

`ldd build/test_cpp_framework_mesh_shutdown_seal`은
`.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so.0`을 가리킨다.
Core·binding 재빌드나 package 변경은 하지 않았다.

첫 vertical 실행의 신규 fixture가 Serving descriptor를 복사해 bind하면서 immutable endpoint
검증에 실패했다. 신규 listener의 초기 상태를 Preparing으로 수정한 뒤 focused·sample self-check·
최종 unit에서 모두 통과했다. Runtime expectation은 완화하지 않았다.

## BLOCKERS

- 남은 실패·구현 blocker 없음. Core·bindings·보호 문서·다른 언어·shared_sample 수정 및 commit 없음.
- .NET D-097과 동일하게 seal은 **Hello 시작**을 막는다. 수신 Hello에 대한 Admit 응답과 이미
  수락한 작업의 completion·liveness 처리는 유지한다. Shutdown seal과 Relocate Draining은 별개다.
