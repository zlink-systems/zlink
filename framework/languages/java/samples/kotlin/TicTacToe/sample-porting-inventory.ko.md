# Kotlin TicTacToe Sample .NET 기준 포팅 Inventory

이 문서는 Kotlin `TicTacToe` 샘플을 `.NET` 기준 구현과 공통 샘플 문서에 맞춰 대조한 결과다.
`pending`이 남아 있으면 이 샘플은 완료로 보지 않는다.

## 기준 파일 대응

| 기준 | Kotlin 대응 | 분류 | 상태 | 비고 |
|------|-------------|------|------|------|
| `.NET: TicTacToe.sln` | `standalone.settings.gradle.kts` | build | done | standalone 실행 시 framework build를 composite build로 참조한다. |
| `.NET: README.md` | `README.md` | docs | done | Kotlin 실행 방식과 Redis 준비 책임을 설명한다. |
| `.NET: run_sample.sh` | `run_sample.sh` | runner | done | Api 2개, Play 2개, Client를 실제 process로 실행한다. |
| `.NET: run_sample.ps1` | `run_sample.ps1` | runner | done | Windows runner도 Bash runner와 같은 role 구성과 Redis endpoint 계약을 사용한다. |
| `.NET: Client/TicTacToe.Client.csproj` | `Client/build.gradle.kts` | build | done | Client role project. |
| `.NET: Client/README.md` | `Client/README.md` | docs | done | client role 설명. |
| `.NET: Client/Program.cs` | `Client/src/main/kotlin/.../client/Program.kt` | client-entry | done | client argument를 읽고 scenario를 실행한다. |
| `.NET: Client/TicTacToeClientScenario.cs` | `Client/src/main/kotlin/.../client/TicTacToeClientScenario.kt` | client-scenario | done | room 생성, stream 인증, host/guest/observer join, move, milestone push를 self-check한다. |
| `.NET: Server/TicTacToe.Server.csproj` | `Server/build.gradle.kts` | build | done | Server role project. |
| `.NET: Server/Program.cs` | `Server/src/main/kotlin/.../server/api/ApiProgram.kt`, `server/play/PlayProgram.kt` | server-entry | done | API와 Play를 별도 실행 진입점으로 시작하며 각 진입점은 설정 파일 경로만 받는다. |
| `.NET: Server/Api/ApiServer.cs` | `Server/src/main/kotlin/.../server/api/ApiServer.kt` | server-role | done | API HTTP/channel role을 실행한다. |
| `.NET: Server/Api/Handlers/AuthenticatePlayerHandler.cs` | `Server/src/main/kotlin/.../api/handlers/AuthenticatePlayerHandler.kt` | handler | done | access token을 player identity로 검증한다. |
| `.NET: Server/Api/Handlers/CreateGameHttpHandler.cs` | `Server/src/main/kotlin/.../api/handlers/CreateGameHttpHandler.kt` | handler | done | HTTP room 생성 요청을 Play channel request로 연결한다. |
| `.NET: Server/Configuration/RedisRoomRouteStore.cs` | `Server/src/main/kotlin/.../configuration/SampleLocationStore.kt` | runtime-config | done | framework Redis location store를 Play role에 등록한다. |
| `.NET: Server/Configuration/SampleFlowLog.cs` | `Server/src/main/kotlin/.../configuration/SampleLogging.kt` | server-evidence | done | role별 flow log를 남기고 runner가 `message flow` marker를 확인한다. |
| `.NET: Server/Configuration/SampleNames.cs` | `Server/src/main/kotlin/.../configuration/SampleNames.kt` | server-config | done | role, service, packet 이름을 공유한다. |
| `.NET: Server/Configuration/SampleSettings.cs` | `Server/src/main/kotlin/.../configuration/SampleSettings.kt` | server-config | done | endpoint, Redis, log 설정을 properties와 args에서 읽는다. |
| `.NET: Server/Play/Application/GameCreation/TicTacToeGameCreator.cs` | `Server/src/main/kotlin/.../play/application/gamecreation/TicTacToeGameCreator.kt` | application-usecase | done | room 응답을 조립하고 game Spot 생성 요청과 분리한다. |
| `.NET: Server/Play/Domain/TicTacToe/TicTacToeBoard.cs` | `Server/src/main/kotlin/.../spots/tictactoegamespot/TicTacToeGame.kt` | domain | done | board, turn, win/draw 판정을 game Spot이 보유한 state로 표현한다. |
| `.NET: Server/Play/Domain/TicTacToe/TicTacToeMatch.cs` | `Server/src/main/kotlin/.../spots/tictactoegamespot/TicTacToeGame.kt` | domain | done | match state와 move 검증을 Spot state operation으로 표현한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Actors/PlayActor.cs` | `Server/src/main/kotlin/.../play/infrastructure/zlink/actors/PlayActor.kt` | actor-adapter | done | bound stream session push를 감싼다. |
| `.NET: Server/Play/Infrastructure/ZLink/Actors/PlayActorFactory.cs` | `Server/src/main/kotlin/.../play/infrastructure/zlink/actors/PlayActorFactory.kt` | actor-adapter | done | Play actor 생성 책임. |
| `.NET: Server/Play/Infrastructure/ZLink/Handlers/CreateGameHandler.cs` | `Server/src/main/kotlin/.../play/infrastructure/zlink/handlers/CreateGameHandler.kt` | channel-handler | done | API의 room 생성 channel request를 application use case로 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Sessions/PlaySession.cs` | `Server/src/main/kotlin/.../play/infrastructure/zlink/sessions/PlaySession.kt` | stream-session | done | stream 인증 후 actor와 bound session을 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Sessions/Handlers/AuthenticatePlaySessionHandler.cs` | `Server/src/main/kotlin/.../sessions/handlers/AuthenticatePlaySessionHandler.kt` | stream-handler | done | stream 인증 request를 API 인증과 actor 준비로 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/PlayEntrySpot.cs` | `Server/src/main/kotlin/.../spots/entryspot/PlayEntrySpot.kt` | spot-adapter | done | actor join, milestone observe, pub/sub event 수신 흐름. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/PlayActorJoinGameHandler.cs` | `Server/src/main/kotlin/.../entryspot/handlers/PlayActorJoinGameHandler.kt` | spot-handler | done | actor를 game Spot에 join한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/PlayActorObserveMilestoneHandler.cs` | `Server/src/main/kotlin/.../entryspot/handlers/PlayActorObserveMilestoneHandler.kt` | spot-handler | done | observer actor를 milestone event 구독 흐름에 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/PlayerWinMilestoneEventHandler.cs` | `Server/src/main/kotlin/.../entryspot/handlers/PlayerWinMilestoneMsgHandler.kt` | pubsub-handler | done | winner milestone event를 observer bound session으로 push한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/TicTacToeGame.cs` | `Server/src/main/kotlin/.../spots/tictactoegamespot/TicTacToeGame.kt` | spot-adapter | done | game Spot lifecycle, player state, move 처리, push, milestone publish를 맡는다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/PlayActorLeaveGameHandler.cs` | `Server/src/main/kotlin/.../tictactoegamespot/handlers/PlayActorLeaveGameHandler.kt` | spot-handler | done | actor leave 처리를 맡는다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/PlayActorPlaceMarkHandler.cs` | `Server/src/main/kotlin/.../tictactoegamespot/handlers/PlayActorPlaceMarkHandler.kt` | spot-handler | done | mark placement request를 game state operation으로 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/TicTacToeGameTimerHandler.cs` | `Server/src/main/kotlin/.../tictactoegamespot/handlers/TicTacToeGameTimerHandler.kt` | timer-handler | done | game Spot timer tick을 처리한다. |
| `.NET: Server/Play/Infrastructure/ZLink/TicTacToeGameRoomProvisioner.cs` | `Server/src/main/kotlin/.../tictactoegamespot/handlers/TicTacToeGameCreatedHandler.kt` | spot-provision | done | created hook에서 room 준비를 완료한다. |
| `.NET: Server/Play/PlayServer.cs` | `Server/src/main/kotlin/.../play/PlayServer.kt` | server-role | done | Play channel, stream, actor, Spot, pub/sub, location-store resolver를 구성한다. |
| `.NET: Shared/TicTacToe.Shared.csproj` | `Shared/build.gradle.kts` | build | done | Shared project. |
| `.NET: Shared/Contracts/Messages.cs` | `Shared/src/main/kotlin/.../shared/contracts/Contracts.kt` | shared-contract | done | request, response, notify, milestone message data class를 둔다. |

## 공통 요구 대응

| 기준 | Kotlin 대응 | 분류 | 상태 | 비고 |
|------|-------------|------|------|------|
| common: 자동 discovery 없이 수동 endpoint 연결 | `run_sample.sh`, `run_sample.ps1`, `SampleSettings.kt` | topology | done | runner가 API/Play endpoint 목록을 properties로 전달한다. |
| common: Api 2개, Play 2개 실행 | `run_sample.sh`, `run_sample.ps1` | runner | done | 실제 process 경계로 role을 실행한다. |
| common: Redis-backed location store | `SampleLocationStore.kt`, `PlayServerApplication.kt` | runtime-config | done | Play role은 `ZLinkRedisLocationStore` bean을 등록하고 framework 기본 resolver가 spot 위치를 조회한다. |
| common: 실행별 전용 Redis 사용 | `run_sample.sh`, `run_sample.ps1` | runner | done | runner가 pinned image로 전용 Docker Redis를 만들고 외부 endpoint를 재사용하지 않는다. |
| common: Docker Redis는 runner 책임 | `run_sample.sh`, `run_sample.ps1` | runner | done | 애플리케이션은 runner가 만든 endpoint만 받고, runner는 자신이 만든 container id만 정리한다. |
| common: 실행별 Redis key prefix 사용 | `run_sample.sh`, `run_sample.ps1` | runner | done | `TICTACTOE_REDIS_KEY_PREFIX`가 없으면 실행별 prefix를 만들고 location store key에 적용한다. |
| common: Redis client dependency는 framework extension 안에 둠 | `zlink-framework-locations-redis`, `SampleLocationStore.kt` | design | done | handler, actor, Spot, Domain에 Redis client 타입을 노출하지 않는다. |
| common: actor가 public Spot API로 room에 join | `PlayEntrySpot.kt`, `PlayActorJoinGameHandler.kt` | spot-flow | done | internal runtime 우회 없이 Spot handler 경로를 사용한다. |
| common: Spot pub/sub milestone fan-out | `PlayEntrySpot.kt`, `PlayerWinMilestoneMsgHandler.kt`, `TicTacToeGame.kt` | pubsub | done | winner milestone event를 pub/sub로 전달하고 observer에게 push한다. |
| common: push 대기는 connector public wait API 사용 | `TicTacToeClientScenario.kt` | validation | done | game start, move, win, milestone notify를 typed wait로 검증한다. |
| common: inbound observer는 connect 전에 등록 | `TicTacToeClientScenario.kt` | validation | done | stream connector 생성 직후 inbound observation을 등록한다. |
| common: inbound observer 로그 확인 | `run_sample.sh`, `run_sample.ps1` | runner | done | observer connection, subscription, milestone marker와 flow log를 확인한다. |
| common: sample-local polling으로 push 대기를 숨기지 않음 | `TicTacToeClientScenario.kt` | validation | done | push 대기는 scenario 코드에 직접 드러난다. |
| common: Domain에는 board, turn, win/draw 판정만 둠 | `Server/src/main/kotlin/.../play/domain/tictactoe/TicTacToeMatch.kt` | design | done | board, turn, win/draw, timeout state 전환은 domain 객체가 맡고 Spot은 framework lifecycle, actor 목록, push를 맡는다. |
| common: Application은 room 생성 use case를 조율 | `TicTacToeGameCreator.kt`, `CreateGameHandler.kt` | design | done | room 응답 조립과 Spot 생성 요청을 조율한다. |
| common: Infrastructure는 HTTP, channel, stream session, actor, Spot, timer, codec 연결을 맡음 | `Server/src/main/kotlin/.../api`, `.../play/infrastructure` | design | done | framework 연결 책임을 infrastructure package가 맡는다. |

## 남은 gap

- 없음. Board, turn, win/draw 판정은 `play/domain/tictactoe/TicTacToeMatch.kt`로 분리했고,
  `TicTacToeGame` Spot은 framework lifecycle과 actor/session push 연결만 맡는다.

## 검증 기록

- 2026-07-07 현재 checkout에서 `nice -n 10 timeout 600s ./run_sample.sh` 통과.
- runner 출력에서 `PASS TicTacToe.Kotlin`을 확인했다.
- runner는 Gradle 호출에 `--no-parallel --max-workers=1`을 사용한다.
- 증거 파일은 `logs/flow-api-50479.log`, `logs/flow-play-node-1.log`, `logs/flow-play-node-2.log`이다.
- flow log에서 `PlayerWinMilestoneMsg` pub/sub fan-out과 양쪽 stream의 `LeaveGameReq` dispatch를 확인했다.
- `rg -n "HANDLER_MISSING|ERROR" framework/languages/java/samples/kotlin/TicTacToe/logs -g '*.log'`는 no-hit이다.
