# GameQuest C++ sample porting inventory

최신 검증: 2026-08-03에 canonical `framework/languages/cpp/build`에서 아래 명령과
six-sample aggregate를 실행했고,
`PASS GameQuest.Cpp`, `gamequest sample result=passed`, `gamequest-server-evidence=completed`,
`gamequest=completed`를 확인했다.

```bash
framework/languages/cpp/samples/GameQuest/run_sample.sh
```

| 기준 | C++ 위치 | 상태 | 설명 |
|------|----------|------|------|
| `.NET Shared/Messages.cs` | `Shared/Contracts/messages.hpp` | done | `GameplayMsg`는 event id·player id·type·typed JSON payload·발생 시각을 최상위 필드로 갖는 응답 없는 메시지다. 별도 envelope나 request/reply wrapper를 사용하지 않으며 projection의 version과 마지막 source event id는 유지한다. |
| `.NET Server/Configuration/SampleConfiguration.cs` | `Server/Configuration/sample_topology.hpp` | done | api-a/api-b, mission-a/mission-b endpoint와 Redis location store prefix를 환경 변수로 받는다. |
| `.NET GameApi session` | `Server/GameApi/main.cpp` | done | stream session이 client request를 받고, 먼저 owner QuestMission에 player quest Spot 생성을 보장한 뒤 public spot route request로 progress sync와 gameplay event 적용을 보낸다. |
| `.NET PlayerQuestSpotProvisioner.cs` | `Server/GameApi/main.cpp`, `Server/QuestMission/main.cpp` | done | C++는 `EnsurePlayerQuestSpotReq` channel request와 `spot_node_manager_t::get_or_create_spot`으로 player owner Spot을 보장한다. |
| `.NET PlayerQuestSpot.cs` | `Server/QuestMission/main.cpp` | done | `player_quest_spot_t`가 player id별 Spot으로 생성되고, gameplay event 적용, progress sync, progress 조회 handler를 소유한다. |
| `.NET QuestMission role` | `Server/QuestMission/main.cpp` | done | mission-a/mission-b가 owner channel과 spot route mesh를 열고, player owner Spot에서 quest projection과 completion notify를 처리한다. |
| `.NET Client/GameQuestClientScenario.cs` | `Client/gamequest_client_scenario.hpp` | done | Alice/Bob gameplay, duplicate idempotency, offline progress sync, completion notify, server evidence를 검증한다. |
| C++ sample runner convention | `run_sample.sh` | done | 필요한 CMake target을 빌드하고, `RUN_DIR`, `stdbuf`, Redis location store, GameApi caller-side spot router, QuestMission spot route/router/pub endpoints, flow trace grep, `ZLINK_CPP_BUILD_DIR` build dir를 사용한다. |

## .NET 파일 대응 보강

| .NET 파일 | C++ 대응 | 상태 | 설명 |
|-----------|----------|------|------|
| `Client/GameQuest.Client.csproj`; `Client/Program.cs`; `README.ko.md`; `Shared/GameQuest.Shared.csproj` | `Client/main.cpp`; `Client/gamequest_client_scenario.hpp`; `README.ko.md`; `Shared/Contracts/messages.hpp`; `framework/languages/cpp/CMakeLists.txt` | done | client entry, scenario, README, shared project/contract target을 C++ client/header/CMake로 대응한다. |
| `Server/Configuration/GameQuest.Server.Configuration.csproj`; `RedisJsonStore.cs`; `SampleFlowLog.cs` | `Server/Configuration/location_store.hpp`; `Server/Configuration/sample_names.hpp`; `Server/Configuration/sample_topology.hpp`; `sample_log_dir.hpp` | done | Redis location store, endpoint/name 설정, flow log 경로를 C++ configuration과 runner log convention으로 대응한다. |
| `Server/GameApi/GameQuest.GameApi.csproj`; `Server/GameApi/Program.cs`; `Session/GameQuestSession.cs`; `Session/GameQuestSessionHandlers.cs`; `Session/GameQuestSessionRegistry.cs` | `Server/GameApi/main.cpp` | done | GameApi executable이 stream session, session registry, gameplay request handling, player owner Spot 보장 요청을 맡는다. |
| `Server/GameApi/Application/GameplayActionService.cs`; `Server/GameApi/Domain/GameplayDomain.cs`; `Infrastructure/Store/GameQuestStores.cs`; `Infrastructure/ZLink/GameplayEventOwnerDispatcher.cs`; `Infrastructure/Http/HttpQuestProgressSynchronizer.cs` | `Server/GameApi/main.cpp`; `Shared/Contracts/messages.hpp` | done | gameplay action/domain, progress sync, owner dispatch, store/projection request는 C++ GameApi role의 typed request flow와 shared DTO로 대응한다. |
| `Server/QuestMission/GameQuest.QuestMission.csproj`; `Server/QuestMission/Program.cs`; `Application/QuestEventProcessor.cs`; `Domain/QuestDomain.cs`; `Infrastructure/Store/QuestStores.cs`; `Infrastructure/Http/GameApiQuestClients.cs` | `Server/QuestMission/main.cpp`; `Shared/Contracts/messages.hpp` | done | QuestMission executable이 shared owner channel, global player Spot, quest aggregate/projection, completion fanout과 server evidence를 맡는다. |

## C++ public API 사용 경계

GameApi와 QuestMission은 공개 framework API만 사용한다. GameApi는 `route_client_t::request_to_node`의
spot 대상 overload로 owner Spot에 요청하고, QuestMission은 `spot_node_manager_t::get_or_create_spot`으로
player Spot을 생성한다. QuestMission 경계에서 payload bytes를 domain fact로 한 번 변환하므로 domain
event store가 wire 형식에 의존하지 않는다. 샘플 코드에서 raw frame, private helper, 메시지별 codec 등록
우회는 사용하지 않는다.

## 계약 검증

- 2026-07-15: `ctest --test-dir framework/languages/cpp/build -R 'test_cpp_framework_sample_parity' --output-on-failure`
  - 결과: 통과
  - 의미: flat `GameplayMsg`, envelope와 request wrapper의 부재, version과 마지막 source event id 유지를 검사한다.
- 2026-07-15: `framework/languages/cpp/samples/GameQuest/run_sample.sh`
  - 결과: 통과
  - 출력: `PASS GameQuest.Cpp`, `gamequest sample result=passed`
  - 의미: flat one-way gameplay 메시지로 idempotency, progress 동기화, completion notify가 끝까지 동작한다.

- 2026-08-03: `framework/languages/cpp/samples/GameQuest/run_sample.sh`
  - 결과: `PASS GameQuest.Cpp`, `gamequest-server-evidence=completed`, `gamequest=completed`
  - 의미: 자동 RouteMesh discovery readiness, typed `GameplayMsg` payload, reconnect, owner
    rehydration, projection reconcile와 scale-out을 실제 client/server process로 확인했다.

## Message 분류

`SyncQuestProgressReq`, `GameplayMsg`, `ClosePlayerQuestMsg`와 client action request/reply는
공통 sample 계약이다. `SyncQuestProgressOwnerReq`와 `NotifyQuestProgressMsg`는 GameApi와
QuestMission 사이의 internal message다. `ProjectionAdmin*`, `GameQuestUnpublishedKill*`와
`GameQuestServerAssert*`는 projection 복구·유실 fact·server evidence를 확인하는
test/evidence-only message다. 이 분류는 public client API를 추가하지 않으며, 사용처가 없는
`NotifyQuestProgressReq/Res` declaration은 제거했다.

## 설계 재검토

기존 envelope를 호환 adapter로 유지하는 안과 wire 메시지를 flat 구조로 바꾸고 QuestMission 경계에서
domain fact로 변환하는 안을 비교했다. adapter를 유지하면 같은 gameplay 의미가 envelope와 flat 형식에
나뉘고 domain event store까지 wire DTO가 전달된다. flat 메시지를 한 번 변환하는 안은 전송 형식을
경계에 가두고 domain event store가 gameplay 규칙만 다루게 하므로 이 방식을 적용했다.
