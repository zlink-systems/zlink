# TicTacToe C++ Sample Porting Inventory

이 문서는 `.NET` TicTacToe 샘플과 공통 TicTacToe 샘플 문서의 요구를 C++ TicTacToe 샘플에
매핑한 inventory다. `.NET` 기준 파일 목록은 `bin`, `obj`, `logs` 산출물을 제외하고 작성한다.
C++ 샘플은 public framework API와 Stream Connector public wait interface를 사용하며, private API,
raw frame 조작, 샘플 전용 route helper로 공통 계약을 우회하지 않는다.

## 기준 파일 매핑

| 기준 | C++ 대응 | 분류 | 상태 | 비고 |
|------|----------|------|------|------|
| `.NET: TicTacToe.sln`, `Server/TicTacToe.Server.csproj`, `Shared/TicTacToe.Shared.csproj`, `Client/TicTacToe.Client.csproj` | `framework/languages/cpp/CMakeLists.txt` | build-root | done | CMake target이 API, Play, Client 실행 파일을 만든다. |
| `.NET: README.md`, `Client/README.md` | `README.ko.md` | sample-doc | done | 역할 구조, 실행 방법, HTTP 시작 흐름, runner self-check 범위를 C++ 기준으로 설명한다. |
| `.NET: run_sample.sh`, `run_sample.ps1` | `run_sample.sh`, `run_sample.ps1` | runner | done | 필요한 CMake target을 빌드하고, CTest gate, Redis 준비, 2 API/2 Play process, client marker, message-flow log를 검증한다. |
| `.NET: Shared/Contracts/Messages.cs` | `Shared/Contracts/messages.hpp` | shared-contract | done | HTTP, channel, stream, actor, Spot payload 이름과 field 의미가 대응한다. |
| `.NET: Client/Program.cs` | `Client/main.cpp` | client-entry | done | API HTTP endpoint를 받고 client scenario를 실행한다. |
| `.NET: Client/TicTacToeClientScenario.cs` | `Client/tictactoe_client_scenario.hpp` | client-scenario | done | room 생성, host/guest/observer stream 연결, join, move, milestone push, final state self-check를 수행한다. |
| `.NET: Server/Program.cs` | `Server/Api/main.cpp`; `Server/Play/main.cpp` | server-entry | done | C++는 API와 Play를 별도 role executable로 실행한다. |
| `.NET: Server/Configuration/SampleNames.cs` | `Client/Configuration/sample_names.hpp`; `Server/Configuration/sample_names.hpp` | configuration | done | channel, Spot, topic, actor 이름이 대응한다. |
| `.NET: Server/Configuration/SampleSettings.cs` | `Client/Configuration/sample_configuration.hpp`; `Server/Configuration/sample_configuration.hpp`; `Server/Configuration/sample_topology.hpp` | configuration | done | endpoint, Redis, role 설정을 C++ CLI/env 설정으로 받는다. |
| `.NET: Server/Configuration/SampleFlowLog.cs` | `Server/sample_log_dir.hpp`; role `main.cpp` trace option | evidence | done | role별 message-flow log 파일 경로를 제공한다. |
| `.NET: Redis Location Store` | `Server/Configuration/location_store.hpp` | external-adapter | done | framework Redis Location Store가 전역 `RoomId`의 current route를 관리한다. sample-local owner route record는 사용하지 않는다. |
| `.NET: Server/Api/ApiServer.cs` | `Server/Api/api_server_host_factory.hpp`; `Server/Api/main.cpp` | server-role | done | HTTP endpoint, API channel server, Play channel client를 구성한다. |
| `.NET: Server/Api/Handlers/AuthenticatePlayerHandler.cs` | `Server/Api/Handlers/authenticate_player_handler.hpp` | handler | done | Play session 인증 요청을 처리하고 user 정보를 반환한다. |
| `.NET: Server/Api/Handlers/CreateGameHttpHandler.cs` | `Server/Api/Handlers/create_game_http_handler.hpp` | handler | done | HTTP room 생성 요청을 Play channel room 생성으로 연결한다. |
| `.NET: Server/Play/PlayServer.cs` | `Server/Play/play_server_host_factory.hpp`; `Server/Play/main.cpp` | server-role | done | Play channel, stream server, actor runtime, SpotNode router/pubsub, Redis resolver를 구성한다. |
| `.NET: Server/Play/Application/GameCreation/TicTacToeGameCreator.cs` | `Server/Play/Application/GameCreation/tictactoe_game_creator.hpp` | application | done | room id 생성, room Spot 생성, Redis room route 기록을 조율한다. |
| `.NET: Server/Play/Domain/TicTacToe/TicTacToeBoard.cs` | `Server/Play/Domain/TicTacToe/tictactoe_match.hpp` | domain | done | board cell, mark, win/draw 판정이 framework 타입 없이 대응한다. |
| `.NET: Server/Play/Domain/TicTacToe/TicTacToeMatch.cs` | `Server/Play/Domain/TicTacToe/tictactoe_match.hpp` | domain | done | player join, turn, move, timeout, snapshot 상태 전이가 대응한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Actors/PlayActor.cs` | `Server/Play/Infrastructure/ZLink/Actors/player_actor.hpp` | actor | done | 인증 user state와 bound session push 책임을 둔다. |
| `.NET: Server/Play/Infrastructure/ZLink/Actors/PlayActorFactory.cs` | `Server/Play/play_server_host_factory.hpp` | actor | done | actor 생성과 dependency 조립을 host factory에서 구성한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Handlers/CreateGameHandler.cs` | `Server/Play/Infrastructure/ZLink/Handlers/create_game_handler.hpp` | handler | done | Play channel room 생성 request를 application use case로 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Sessions/PlaySession.cs` | `Server/Play/Infrastructure/ZLink/Sessions/play_session.hpp` | session | done | stream 인증, actor binding, actor relay를 맡는다. |
| `.NET: Server/Play/Infrastructure/ZLink/Sessions/Handlers/AuthenticatePlaySessionHandler.cs` | `Server/Play/Infrastructure/ZLink/Sessions/Handlers/authenticate_play_session_handler.hpp` | handler | done | stream 인증 packet을 API channel 인증 요청으로 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/PlayEntrySpot.cs` | `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/tictactoe_entry_spot.hpp` | spot | done | actor entry lifecycle, room join, observer milestone subscription을 맡는다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/PlayActorJoinGameHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play_actor_join_game_handler.hpp` | spot-handler | done | actor room join request를 room Spot join으로 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/PlayActorObserveMilestoneHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play_actor_observe_milestone_handler.hpp` | spot-handler | done | observer actor를 local Entry Spot milestone observer로 등록한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/PlayerWinMilestoneEventHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/player_win_milestone_event_handler.hpp` | pubsub-handler | done | milestone pub/sub event를 observer bound session push로 바꾼다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/TicTacToeGame.cs` | `Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe_game_spot.hpp`; `tictactoe_game_contract_mapper.hpp`; `tictactoe_game_models.hpp` | spot | done | room Spot lifecycle, join/leave, move, timer, domain 호출, push 전송이 대응한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/PlayActorLeaveGameHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play_actor_leave_game_handler.hpp` | spot-handler | done | `LeaveGameMsg`는 reply 없는 actor send로 등록하고 Entry Spot 복귀 흐름을 실행한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/PlayActorPlaceMarkHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play_actor_place_mark_handler.hpp` | spot-handler | done | mark request를 domain state 변경으로 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/TicTacToeGameTimerHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/tictactoe_game_spot_created_handler.hpp` | timer-handler | done | game Spot 생성 뒤 timer 등록과 timeout 흐름이 대응한다. |
| `.NET: Server/Play/Infrastructure/ZLink/TicTacToeGameRoomProvisioner.cs` | `Server/Play/Application/GameCreation/tictactoe_game_creator.hpp`; `Server/Play/Infrastructure/ZLink/Handlers/create_game_handler.hpp`; `Server/Configuration/location_store.hpp` | infrastructure | done | public Spot manager가 전역 `RoomId`를 get-or-create하고 Location Store가 current route를 관리한다. |

## 공통 요구 매핑

| 기준 | C++ 대응 | 분류 | 상태 | 비고 |
|------|----------|------|------|------|
| `common: 2 API, 2 Play 수동 endpoint scale-out` | `run_sample.sh`; `run_sample.ps1`; `Server/Api`; `Server/Play` | validation | done | runner가 api-a, api-b, play-a, play-b를 별도 process로 띄운다. |
| `common: client는 API 응답의 Play stream endpoint를 사용` | `Client/tictactoe_client_scenario.hpp`; `Server/Api/Handlers/create_game_http_handler.hpp` | client-flow | done | client는 API HTTP endpoint만 입력으로 받고 Play stream endpoint는 `CreateGameHttpRes`에서 읽는다. |
| `common: JSON payload` | `Shared/Contracts/messages.hpp`; `Server/Play/Infrastructure/ZLink/Sessions/play_session.hpp` | codec | done | stream, channel, actor, Spot payload는 typed JSON message 경로를 사용한다. |
| `common: Redis Location Store` | `Server/Configuration/location_store.hpp`; `run_sample.sh`; `run_sample.ps1` | external-adapter | done | Redis Location Store는 전역 `RoomId`의 현재 route를 저장한다. sample은 physical owner NodeRid를 따로 계산하거나 저장하지 않는다. |
| `common: runner가 Docker Redis 준비` | `run_sample.sh`; `run_sample.ps1` | runner | done | 전용 Redis container를 만들고 cleanup에서 제거한다. 외부 Redis endpoint를 받아 로컬 Redis나 공유 Redis를 건드리지 않는다. |
| `common: Redis key prefix 격리` | `run_sample.sh`; `run_sample.ps1`; `Server/Configuration/sample_topology.hpp` | runner | done | 실행마다 고유한 `TICTACTOE_CPP_REDIS_KEY_PREFIX` 기본값을 전달한다. |
| `common: remote Spot join은 public resolver 사용` | `Server/Configuration/location_store.hpp`; `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/tictactoe_entry_spot.hpp` | message-flow | done | 전역 `RoomId`를 public actor/Spot join 경로에 넘기며 Location Store가 현재 owner route를 해석한다. |
| `common: Spot pub/sub milestone fan-out` | `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/tictactoe_entry_spot.hpp`; `Client/tictactoe_client_scenario.hpp` | message-flow | done | room Spot publish와 Entry Spot subscribe handler로 observer milestone push를 검증한다. |
| `common: public connector wait interface로 push 대기` | `Client/tictactoe_client_scenario.hpp` | validation | done | wait filter와 future를 직접 사용하고 sample-local polling으로 push 대기를 숨기지 않는다. |
| `common: tictactoe=completed marker` | `Client/main.cpp`; `run_sample.sh`; `run_sample.ps1` | validation | done | runner가 client log marker를 검사한다. |
| `common: stream-inbound marker와 message-flow evidence` | `Client/main.cpp`; `run_sample.sh`; `run_sample.ps1`; role `main.cpp` trace option | validation | done | runner가 client marker, `LeaveGameMsg` 완료, Entry Spot destroy 완료, sample log directory message-flow 기록을 검사한다. |
| `common: Domain은 framework 타입을 모름` | `Server/Play/Domain/TicTacToe/tictactoe_match.hpp` | layering | done | board, turn, win/draw 판정은 domain type에 있고 framework 배선은 Infrastructure에 있다. |

## 남은 gap

현재 sample process와 static contract 기준의 pending 항목은 없다. `CreateGameHttpReq.gameName`
의 nullable 표현, `CreateGameHttpRes`의 `playEndpoints`·`playNodes`, `JoinGameFailedNotify`와
`LeaveGameMsg`를 확인했다. `CreateGameReq`와 `TicTacToeGameJoin*`는 API와 Play 사이의
internal message이며 public client contract에 추가하지 않는다. 전체 C++ S1 closure와 common
E2E ID 추적은 이 sample inventory의 범위를 넘으므로 ledger에서 별도로 판정한다.

## 현재 계약·process evidence

- `LeaveGameMsg`는 actor send로 등록되고 reply를 기다리지 않는다.
- API별 flow trace를 `api-a`와 `api-b` 파일로 분리해 source, dispatch, reply와 terminal
  결과를 확인한다.
- 2026-08-03 개별 runner 8회와 official six-sample aggregate가 모두 exit code 0이다.
  최신 aggregate log는 `/tmp/zlink-cpp-official-sample-aggregate-final-20260803.log`다.
