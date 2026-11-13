# #1639 3단계 검토 수정

**결과**

- 누락 채널은 `mesh_record_dispatcher_t`의 miss에서 기존 `route_packet_dispatcher_t`로 전달해 기존 no-handler 규칙이 `not_found` 응답과 send 보고를 처리합니다 (`framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:75-93`, `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:241-250,351-361`).
- 일반 Node send는 스택 terminal owner를 유지하고 dispatcher에는 빈 terminal을 넘깁니다. Spot promotion 경로는 변경하지 않았습니다 (`framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2323-2352`, `framework/languages/cpp/framework/src/runtime/mesh/application_dispatch_terminal_owner.hpp:55-60`).
- 스펙은 Node handler에 execution gate가 없고 suspend 중 다른 pipeline이 실행될 수 있다고 정하지만, 이 mailbox terminal의 settle 시점을 handler completion으로 늦추라는 조항은 찾지 못했습니다 (`framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:97-104`).
- 변경 분류: B, 기존 회귀 수정. 다른 언어의 미등록 request도 framework `NOT_FOUND` 오류로 끝납니다 (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:560-573`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkMeshApplicationDispatcher.java:606-625`).

**결정 소유 표 및 규칙 수**

| 결정 | 단일 소유자 | 판정 위치 수(전→후) | 규칙 수(전→후) |
|---|---|---:|---:|
| 계획에 없는 채널의 dispatch | `mesh_record_dispatcher_t`; 결과는 기존 `route_packet_dispatcher_t`가 결정 | 1→1 | 1→1 |
| 일반 Node send terminal settle 시점 | `node_receive_role_t` 스택 소유; Spot promotion은 Spot runtime | 1→1 | 1→1 |
| 합계 |  | 2→2 | 2→2 |

**검증**

- 채널 회귀: 3db0980890에서 유효한 fixture로 실행 시 exit 2; 수정 후 `test_cpp_framework_channel_messaging` 50회 통과 (`logs/channel-head-valid-run2.status`, `logs/channel-after-final-50.log`).
- Node terminal: suspend 중인 Node send handler를 두고 공통 스택-owner helper의 callback이 handler resume 전에 실행됨을 확인했습니다 (`framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_node_vertical.cpp:1566-1624`, `logs/terminal-after-final2.log`, exit 0). 테스트가 주입한 callback은 production Core mailbox callback 자체를 가로채지 않습니다. 첫 테스트 실행은 fixture에 completion owner가 없어 실패했고, fixture를 보완해 통과했습니다 (`framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:143-148`). Terminal 회귀의 HEAD 실패 재현은 유효하게 확보하지 못했습니다.
- 전체 C++ unit+contract: 292개 중 291개 통과. 유일한 tooling contract 실패는 `USE_BINDINGS_SOURCE=ON` 설정에서 별도 configure가 `zlink_cpp` 설치 prefix를 찾지 못한 환경 문제였습니다. 소스 빌드 binding을 임시 prefix에 설치한 뒤 해당 tooling contract를 단독 재실행해 통과했습니다 (`logs/framework-unit-contract-ctest.log`, `logs/tooling-contract-rerun.status`).
- Release 전체 빌드 완료(493/493), 변경 타깃 재빌드 통과, `bash scripts/format/format.sh --check cpp` 통과, `git diff --check` 통과. 로그는 `/root/jobs/fx/cx/v21/s033/st3b/logs/`에 보존했습니다.

**커밋**

- `e1131451c5` — `fix(cpp): preserve mesh route miss and node send timing`
- 빌드 디렉터리와 임시 binding prefix를 제거했습니다. Push하지 않았습니다.
