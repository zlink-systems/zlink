# #1639 stage 3 — Node send terminal 회귀 테스트

**상태**

- 요청한 검증 조건을 충족하지 못해 커밋하지 않았습니다.
- terminal helper를 제거하고 `node_receive_role_t`의 dispatch 주변 block scope에서 terminal을 소유하도록 인라인했습니다.
- 수신 Node handler가 멈춘 동안 실제 두-peer Node send 경로에서 다음 send가 처리되는 vertical 테스트로 교체했습니다. HEAD에서는 20/20 통과했지만 3db0980890에서도 통과해 terminal 변경을 판별하지 못합니다.

**검증 결과**

- 3db0980890 제품 파일 + 테스트: 빌드 성공, 테스트 exit 0 — [vertical-red-two-sends.log](/root/jobs/fx/cx/v21/s033/st3c/logs/vertical-red-two-sends.log).
- HEAD 제품 파일 + 같은 테스트: exit 0, 20/20 — [repeat-summary.log](/root/jobs/fx/cx/v21/s033/st3c/logs/repeat-summary.log).
- Channel messaging: 1회차 exit 2. 임시 진단으로 `test_cpp_framework_channel_messaging.cpp:969`의 미등록 채널 request 회귀 assertion이 첫 실패임을 확인했습니다 — [channel-head-repeat-1.log](/root/jobs/fx/cx/v21/s033/st3c/logs/channel-head-repeat-1.log), [channel-diagnostic.log](/root/jobs/fx/cx/v21/s033/st3c/logs/channel-diagnostic.log). 요청된 20회 반복은 중단했습니다.
- 다른 주입 시도는 direct dispatch를 타지 못하거나 handler 진입 전에 실패해 사용하지 않았습니다. 제품 test hook은 추가하지 않았습니다.
- 테스트 코드의 임시 진단 출력은 제거했습니다. `git diff --check` 통과.

**변경 파일**

- `framework/languages/cpp/framework/src/runtime/mesh/application_dispatch_terminal_owner.hpp` — 직접 helper와 미사용 include 제거.
- `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp` — direct terminal을 dispatch 호출을 감싸는 block-local 값으로 이동.
- `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_node_vertical.cpp` — production peer receive 경로의 suspending Node send 테스트.

**미완료**

- 3db0980890에서 실패하고 HEAD에서 통과하는 terminal 소유 경로 테스트 fixture가 필요합니다. 현재 확보한 두-peer 테스트는 회귀 판별자가 아니므로 변경을 커밋하지 않았습니다.
- Channel messaging의 첫 실패는 이전 단계의 route-miss request assertion입니다. 이번 단계 범위에서 수정하지 않았습니다.
