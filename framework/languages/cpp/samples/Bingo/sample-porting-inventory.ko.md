# Bingo C++ Sample Porting Inventory

이 문서는 `.NET` Bingo 샘플과 공통 Bingo 샘플 문서의 요구를 C++ Bingo 샘플에 매핑한
inventory다. C++ 샘플은 public framework API와 Stream Connector public wait interface를
사용하며, public contract에 없는 항목을 private API나 raw frame 조작으로 우회하지 않는다.

## 기준 파일 매핑

| 기준 | C++ 대응 | 분류 | 상태 | 비고 |
|------|----------|------|------|------|
| `.NET: Bingo.csproj`, `Bingo.sln`, role별 project와 Shared project | `framework/languages/cpp/CMakeLists.txt` | build-root | done | CMake target이 api/matchmaking/play/session/client 실행 파일과 shared/configuration header 경로를 만든다. |
| `.NET: Shared/Contracts/bingo_messages.proto` | `Shared/Contracts/messages.hpp`; `Server/common_codecs.hpp` | shared-contract | done | `.NET` proto message 이름과 field 의미를 C++ typed message로 대응하고, C++ framework Protobuf codec extension serializer로 등록한다. |
| `.NET: Shared/Contracts/SampleConstants.cs` | `Shared/Contracts/messages.hpp`, `Server/Configuration/sample_names.hpp` | shared-contract | done | packet 이름, player id, mode, reward 상수를 C++ public message/header로 대응한다. |
| `.NET: Client/Program.cs` | `Client/main.cpp` | client-entry | done | Session stream connector 세 개를 만들고 client scenario를 실행한다. |
| `.NET: Client/Configuration/SampleNames.cs` | `Client/Configuration/sample_topology.hpp`, `Client/Configuration/sample_configuration.hpp` | client-config | done | client endpoint와 sample 설정을 C++ CLI/env 설정으로 받는다. |
| `.NET: Client/BingoClientScenario.cs` | `Client/bingo_client_scenario.hpp` | client-scenario | done | authenticate, match, observe, submit, draw, reward, stop-observe self-check를 수행한다. |
| `.NET: Server/Configuration/SampleNames.cs` | `Server/Configuration/sample_names.hpp` | server-config | done | service, channel, stream, spot 이름을 서버 역할에서 공유한다. |
| `.NET: Server/Configuration/SampleTopology.cs` | `Server/Configuration/sample_topology.hpp` | server-config | done | API의 Play·Matchmaking Object Client endpoint, Matchmaking·Play·Session endpoint와 Redis 설정을 제공한다. 업무 object 배치에 NodeRid를 사용하지 않는다. |
| `.NET: Server/Configuration/SampleFlowLog.cs` | `Server/sample_log_dir.hpp`, role별 `main.cpp` | server-evidence | done | message-flow log를 sample logs 디렉터리에 남긴다. |
| `.NET: Server/Registry/Program.cs` | not-needed | server-role | removed | C++ Bingo는 registry process 대신 Redis `redis_location_store_t`를 등록한다. |
| `.NET: Server/Registry/RegistryHostFactory.cs` | not-needed | server-role | removed | registry host factory는 삭제했다. 서버 간 endpoint 발견은 framework location store가 맡는다. |
| `.NET: Server/Api/Program.cs` | `Server/Api/main.cpp` | server-role | done | API 역할을 별도 process로 실행한다. |
| `.NET: Server/Api/ApiServerHostFactory.cs` | `Server/Api/api_server_host_factory.hpp`, `Server/Api/api_server_framework.hpp` | server-role | done | API channel server와 Play channel client를 구성한다. |
| `.NET: Server/Api/Handlers/AuthenticatePlayerHandler.cs` | `Server/Api/Handlers/authenticate_player_handler.hpp` | handler | done | access token을 확인하고 전역 ActorId와 display name을 반환한다. Actor owner는 선택하지 않는다. |
| `.NET: Server/Api/Handlers/MatchBingoHandler.cs` | `Server/Api/Handlers/match_bingo_handler.hpp` | handler | done | Matchmaker Instance Spot에서 reservation을 받은 뒤 Play Room User Spot `GetOrCreate`가 Ready가 될 때까지 기다린다. |
| `.NET: Server/Matchmaking` | `Server/Matchmaking` | server-role | done | `bingo.matchmaker` Instance Spot과 Redis reservation adapter를 별도 process에 둔다. |
| `.NET: Server/Session/Program.cs` | `Server/Session/main.cpp` | server-role | done | Session stream gateway를 별도 process로 실행한다. |
| `.NET: Server/Session/SessionServerHostFactory.cs` | `Server/Session/session_server_host_factory.hpp`, `Server/host_support.hpp` | server-role | done | stream endpoint, session Spot node, actor relay를 구성한다. |
| `.NET: Server/Session/Sessions/BingoSession.cs` | `Server/Session/Sessions/bingo_session.hpp` | session | done | stream 인증, actor binding, bound actor relay 책임을 둔다. |
| `.NET: Server/Session/Sessions/Handlers/AuthenticateSessionHandler.cs` | `Server/Session/Sessions/Handlers/authenticate_session_handler.hpp` | handler | done | Session 인증 packet을 처리하고 actor binding으로 연결한다. |
| `.NET: Server/Play/Program.cs` | `Server/Play/main.cpp` | server-role | done | Play 역할을 `play-a`, `play-b` 별도 process로 실행한다. |
| `.NET: Server/Play/PlayServerHostFactory.cs` | `Server/Play/play_server_host_factory.hpp`, `Server/host_support.hpp` | server-role | done | actor runtime, Spot node, Play channel server, pub/sub, Redis adapter를 구성한다. |
| `.NET: Server/Matchmaking/Application/IBingoMatchReservationStore.cs` | `Server/Matchmaking/Application/bingo_match_reservation_store.hpp` | application | done | Matchmaker Instance Spot이 의존하는 reservation Store 계약이다. |
| `.NET: Server/Play/Domain/Bingo/BingoCard.cs` | `Server/Play/Domain/Bingo/bingo_card.hpp` | domain | done | 3 x 3 card, free cell, mark, complete line 계산을 맡는다. |
| `.NET: Server/Play/Domain/Bingo/BingoGame.cs` | `Server/Play/Domain/Bingo/bingo_game.hpp` | domain | done | draw deck, drawn numbers, winner 판정을 맡는다. |
| `.NET: Server/Play/Domain/Bingo/BingoRoomGame.cs` | `Server/Play/Domain/Bingo/bingo_room_game.hpp` | domain | done | player join, card 제출, draw 시작/종료 신호를 맡는다. |
| `.NET: Server/Play/Domain/Bingo/BingoRoomModels.cs` | `Server/Play/Domain/Bingo/bingo_room_game.hpp`, `Shared/Contracts/messages.hpp` | domain | done | room state와 player state 모델을 C++ 타입으로 대응한다. |
| `.NET: Server/Matchmaking/Infrastructure/Redis/RedisBingoMatchReservationStore.cs` | `Server/Matchmaking/Infrastructure/Redis/redis_bingo_match_reservation_store.hpp` | external-adapter | done | Redis Lua operation이 waiting room, 동일 settings와 actor reservation을 원자적으로 결정한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Actors/PlayerActor.cs` | `Server/Play/Infrastructure/ZLink/Actors/player_actor.hpp` | actor | done | bound session push를 actor public method로 감싼다. |
| `.NET: Server/Play/Infrastructure/ZLink/Actors/PlayerActorFactory.cs` | `Server/Play/Infrastructure/ZLink/Actors/player_actor_factory.hpp` | actor | done | actor 생성과 dependency 조립을 맡는다. |
| `.NET: player actor 준비와 session bind` | `Server/Session/Sessions/Handlers/authenticate_session_handler.hpp`; `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp` | actor-lifecycle | done | Session이 public global actor `get_or_create` 결과의 exact ref를 bind하고 Entry Spot lifecycle이 create request를 적용한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/BingoEntrySpot.cs` | `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp` | spot | done | actor가 room에 들어가기 전 admission 지점을 맡는다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/MatchBingoActorHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/match_bingo_actor_handler.hpp` | spot-handler | done | actor matching 요청을 room allocation과 room join으로 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/ObserveBingoEventsHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/observe_bingo_events_handler.hpp` | spot-handler | done | observer actor를 observer용 local room으로 join시킨다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/BingoRoom.cs` | `Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp` | spot | done | room lifecycle, actor join/leave, draw, domain 호출, pub/sub 처리를 맡는다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/BingoRoomSettingsPayloadMapper.cs` | `Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp` | spot | done | C++는 room settings payload 변환을 room Spot header 안에서 처리한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/SubmitBingoCardHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/submit_bingo_card_handler.hpp` | spot-handler | done | card 제출 요청을 domain state 변경으로 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/BingoRoomDrawTimerHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo_room_draw_timer_handler.hpp` | timer-handler | done | room Spot timer가 200ms 간격으로 번호를 하나씩 추첨한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/BingoRewardAcquiredEventHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo_reward_acquired_event_handler.hpp` | pubsub-handler | done | reward event 수신 후 observer actor에게 push한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/StopObservingBingoEventsHandler.cs` | `Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/stop_observing_bingo_events_handler.hpp` | spot-handler | done | observer actor를 observer용 room에서 내보낸다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Notifications/BingoNotificationPublisher.cs` | `Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp` | notification | done | C++는 별도 publisher class 없이 room Spot에서 public publish API를 호출한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Notifications/BingoRoomEvent.cs` | `Shared/Contracts/messages.hpp` | notification | done | reward event payload를 shared message 타입으로 대응한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Notifications/BingoRoomEventMapper.cs` | `Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp` | notification | done | domain event를 reward notify payload로 변환한다. |
| `.NET: run_sample.sh`, `run_sample.ps1` | `run_sample.sh`, `run_sample.ps1` | runner | done | 필요한 CMake target을 빌드하고, process readiness, Redis 준비, client marker, message-flow log, player actor destroy evidence를 검증한다. |
| `.NET: README.md` | `README.ko.md` | sample-doc | done | C++ 역할 구조, 실행 방법, Redis 실행 책임을 설명한다. |

## 공통 요구 매핑

| 기준 | C++ 대응 | 분류 | 상태 | 비고 |
|------|----------|------|------|------|
| `common: client는 Session stream endpoint 하나만 직접 연결` | `Client/main.cpp`, `Client/bingo_client_scenario.hpp`, `run_sample.sh` | validation | done | client는 session-a/session-b stream endpoint만 받는다. |
| `common: API 2개, Matchmaking 1개, Session 2개, Play 2개 실행` | `run_sample.sh` | validation | done | Linux runner가 일곱 server process를 분리해 시작한다. Matchmaking 개수는 샘플 규모이며 singleton 계약이 아니다. |
| `common: API 인증과 matching 요청` | `Server/Api/Handlers/authenticate_player_handler.hpp`, `Server/Api/Handlers/match_bingo_handler.hpp` | message-flow | done | API channel handler가 인증과 match 요청을 맡는다. |
| `common: Session 인증, actor binding, packet relay` | `Server/Session/Sessions/bingo_session.hpp`, `Server/Session/Sessions/Handlers/authenticate_session_handler.hpp` | message-flow | done | session은 gateway 책임만 갖고 게임 규칙을 해석하지 않는다. |
| `common: Play actor 생성과 Entry Spot join` | `Server/Session/Sessions/Handlers/authenticate_session_handler.hpp`; `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp` | message-flow | done | global actor 생성, exact ref session bind와 Entry Spot lifecycle이 public framework 경로에 있다. |
| `common: Redis-backed reservation` | `Server/Matchmaking/Infrastructure/Redis/redis_bingo_match_reservation_store.hpp` | external-adapter | done | Redis client dependency와 atomic reservation script는 Matchmaking adapter 안에 있다. |
| `common: remote Spot join은 location store 기반 resolver 사용` | `Server/Play/play_server_host_factory.hpp`, `Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp`, `Server/Configuration/location_store.hpp` | message-flow | done | room owner가 다른 Play node일 때 public Spot join 경로를 사용하고 Redis location store로 target Spot 위치를 찾는다. |
| `common: Spot pub/sub reward fan-out` | `Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp` | message-flow | done | `bingo.room.reward` 의미의 reward event를 room Spot에서 publish/subscribe한다. |
| `common: payload codec` | `Shared/Contracts/messages.hpp`, `Server/common_codecs.hpp`, `Server/Session/Sessions/bingo_session.hpp`, `Client/main.cpp` | codec | done | stream, channel, actor, Spot payload는 C++ framework Protobuf codec extension과 `application/x-protobuf` stream codec 경로를 사용한다. |
| `common: public connector wait interface로 push 대기` | `Client/bingo_client_scenario.hpp` | validation | done | wait filter와 future를 직접 사용하고 sample-local inbox로 숨기지 않는다. |
| `common: `bingo=completed` marker` | `Client/main.cpp`, `run_sample.sh` | validation | done | runner가 client log marker를 검사한다. |
| `common: stream-inbound marker와 Notify marker` | `Client/main.cpp`, `run_sample.sh` | validation | done | inbound observer log와 notify 수신 marker를 검사한다. |
| `common: message-flow server evidence` | `Server/sample_log_dir.hpp`, `run_sample.sh`, `run_sample.ps1` | validation | done | runner가 sample log directory에서 message-flow log를 검사하고 player actor destroy 완료와 observer 미-destroy 조건을 확인한다. |
| `common: runner가 Docker Redis 준비` | `run_sample.sh` | runner | done | 전용 Redis container를 만들고 cleanup에서 제거한다. 외부 Redis endpoint를 받아 로컬 Redis나 공유 Redis를 건드리지 않는다. |
| `common: Redis key prefix 격리` | `run_sample.sh`, `Server/Configuration/sample_topology.hpp` | runner | done | 실행마다 고유한 `BINGO_REDIS_KEY_PREFIX`를 전달한다. |
| `common: compact 구현 금지` | `Server/Api`, `Server/Matchmaking`, `Server/Session`, `Server/Play` | structure | done | 단일 `--role` 실행 파일이 아니라 역할별 실행 파일로 분리되어 있다. |
| `common: Domain은 framework 타입을 모름` | `Server/Play/Domain/Bingo/*.hpp` | layering | done | card, game, room rule 타입을 Play domain 아래에 둔다. |

## 남은 gap

현재 Bingo sample의 Protobuf contract와 Linux multi-process process gate는 통과했다. 인증 실패
응답, 관찰 room의 nullable `lastDrawnNumber`와 optional `observedRoomId`를 typed Protobuf
payload로 확인하며, `EnsurePlayerActorReq`는 Session과 Play 사이의 내부 request다. 공통 문서에
없는 응답 type을 다시 추가하지 않는다. native Windows runner, package provenance와 common
E2E 14-config 전체 추적은 아직 별도 gate다.

## 현재 검증

- 2026-08-03: 개별 Bingo runner와 official six-sample aggregate가 exit code 0이다.
- 2026-08-03: PowerShell aggregate도 Linux에서 같은 six-sample manifest를 호출해 exit code
  0을 반환했다. 이 결과는 native Windows process 검증으로 승격하지 않는다.
