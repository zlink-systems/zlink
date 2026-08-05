# Kotlin Bingo Sample .NET 기준 포팅 Inventory

이 문서는 Kotlin `Bingo` 샘플을 `.NET` 기준 구현과 공통 샘플 문서에 맞춰 대조한 결과다.
`pending`이 남아 있으면 이 샘플은 완료로 보지 않는다.

## 기준 파일 대응

| 기준 | Kotlin 대응 | 분류 | 상태 | 비고 |
|------|-------------|------|------|------|
| `.NET: Bingo.csproj` | `build.gradle.kts` | build | done | Kotlin 루트는 하위 role project를 묶는다. |
| `.NET: Bingo.sln` | `standalone.settings.gradle.kts` | build | done | standalone 실행 시 framework build를 composite build로 참조한다. |
| `.NET: README.md` | `README.md` | docs | done | Kotlin 실행 방식과 Redis 준비 책임을 설명한다. |
| `.NET: run_sample.sh` | `run_sample.sh` | runner | done | Api 2개, Session 2개, Play 2개, Client를 실제 process로 실행하고 위치 정보는 Redis store에 둔다. |
| `.NET: run_sample.ps1` | `run_sample.ps1` | runner | done | Windows runner도 같은 role 구성과 Redis endpoint 계약을 사용한다. |
| `.NET: Client/Bingo.Client.csproj` | `Client/build.gradle.kts` | build | done | Client role project. |
| `.NET: Client/Program.cs` | `Client/src/main/kotlin/.../client/Program.kt` | client-entry | done | 세 stream connector를 만들고 scenario에 넘긴다. |
| `.NET: Client/BingoClientScenario.cs` | `Client/src/main/kotlin/.../client/BingoClientScenario.kt` | client-scenario | done | 인증, matching, observer, card 제출, draw, 종료, reward, stop-observe를 self-check한다. |
| `.NET: Client/Configuration/SampleNames.cs` | `Client/src/main/kotlin/.../client/configuration/SampleNames.kt` | client-config | done | packet 이름과 sample marker를 client에서 사용한다. |
| `.NET: Server/Configuration/Bingo.Server.Configuration.csproj` | `Server/Configuration/build.gradle.kts` | build | done | 서버 공통 설정 project. |
| `.NET: Server/Configuration/SampleFlowLog.cs` | `Server/*` role logs + `BINGO_LOG_DIR` | server-evidence | done | Kotlin은 role별 application에서 flow log를 남기고 runner가 `message flow` marker를 확인한다. |
| `.NET: Server/Configuration/SampleNames.cs` | `Server/Configuration/src/main/kotlin/.../configuration/SampleNames.kt` | server-config | done | role, service, packet 이름을 공유한다. |
| `.NET: Server/Configuration/SampleTopology.cs` | `Server/Configuration/src/main/kotlin/.../configuration/SampleTopology.kt` | server-config | done | endpoint와 Redis 설정을 system property에서 읽는다. |
| location store 설정 | `Server/Configuration/src/main/kotlin/.../configuration/SampleLocationStore.kt` | server-config | done | 공식 Redis location store extension을 생성하고 sample Redis prefix 아래에서 위치 정보를 분리한다. |
| `.NET: Server/Api/Bingo.Server.Api.csproj` | `Server/Api/build.gradle.kts` | build | done | Api role project. |
| `.NET: Server/Api/Program.cs` | `Server/Api/src/main/kotlin/.../api/Program.kt` | server-entry | done | Api role 단독 entry point. |
| `.NET: Server/Api/ApiServerHostFactory.cs` | `Server/Api/src/main/kotlin/.../api/ApiServerApplication.kt` | server-role | done | Api channel server와 Play channel client를 구성한다. |
| `.NET: Server/Api/Handlers/AuthenticatePlayerHandler.cs` | `Server/Api/src/main/kotlin/.../api/handlers/AuthenticatePlayerHandler.kt` | handler | done | access token을 player identity로 검증한다. |
| `.NET: Server/Api/Handlers/MatchBingoHandler.cs` | `Server/Api/src/main/kotlin/.../api/handlers/MatchBingoHandler.kt` | handler | done | matching API 요청을 Play room allocation으로 연결한다. |
| `.NET: Server/Session/Bingo.Server.Session.csproj` | `Server/Session/build.gradle.kts` | build | done | Session role project. |
| `.NET: Server/Session/Program.cs` | `Server/Session/src/main/kotlin/.../session/Program.kt` | server-entry | done | Session role 단독 entry point. |
| `.NET: Server/Session/SessionServerHostFactory.cs` | `Server/Session/src/main/kotlin/.../session/SessionServerApplication.kt` | server-role | done | stream server, session Spot node, Api channel client를 구성한다. |
| `.NET: Server/Session/Sessions/BingoSession.cs` | `Server/Session/src/main/kotlin/.../session/sessions/BingoSession.kt` | stream-session | done | 인증 후 actor binding과 packet relay를 맡는다. |
| `.NET: Server/Session/Sessions/Handlers/AuthenticateSessionHandler.cs` | `Server/Session/src/main/kotlin/.../session/sessions/handlers/AuthenticateSessionHandler.kt` | stream-handler | done | stream 인증 request를 Api 인증과 actor 준비로 연결한다. |
| `.NET: Server/Play/Bingo.Server.Play.csproj` | `Server/Play/build.gradle.kts` | build | done | Play role project. |
| `.NET: Server/Play/Program.cs` | `Server/Play/src/main/kotlin/.../play/Program.kt` | server-entry | done | Play role 단독 entry point. |
| `.NET: Server/Play/PlayServerHostFactory.cs` | `Server/Play/src/main/kotlin/.../play/PlayServerApplication.kt` | server-role | done | actor, Entry Spot, room Spot, Play channel, Redis adapter를 구성한다. |
| `.NET: Server/Play/Application/RoomAllocation/IBingoMatchQueue.cs` | `Server/Play/src/main/kotlin/.../application/roomallocation/BingoMatchQueue.kt` | application-port | done | room allocation use case가 Redis 세부 구현을 모르도록 계약을 둔다. |
| `.NET: Server/Play/Application/RoomAllocation/BingoRoomAllocator.cs` | `Server/Play/src/main/kotlin/.../application/roomallocation/BingoRoomAllocator.kt` | application-usecase | done | waiting room reservation과 allocation 결과 생성을 조율한다. |
| `.NET: Server/Play/Infrastructure/Redis/RedisBingoMatchQueue.cs` | `Server/Play/src/main/kotlin/.../infrastructure/zlink/matchmaking/RedisBingoMatchQueue.kt` | external-adapter | done | Redis-backed match queue adapter. |
| `.NET: Server/Play/Domain/Bingo/BingoCard.cs` | `Server/Play/src/main/kotlin/.../domain/bingo/BingoCard.kt` | domain | done | card 검증과 mark 계산. |
| `.NET: Server/Play/Domain/Bingo/BingoGame.cs` | `Server/Play/src/main/kotlin/.../domain/bingo/BingoGame.kt` | domain | done | draw deck과 game state. |
| `.NET: Server/Play/Domain/Bingo/BingoRoomGame.cs` | `Server/Play/src/main/kotlin/.../domain/bingo/BingoRoomGame.kt` | domain | done | player join, card submit, draw, winner 판정. |
| `.NET: Server/Play/Domain/Bingo/BingoRoomModels.cs` | `Server/Play/src/main/kotlin/.../domain/bingo/BingoRoomModels.kt` | domain | done | room settings, player, event model. |
| `.NET: Server/Play/Infrastructure/ZLink/Actors/PlayerActor.cs` | `Server/Play/src/main/kotlin/.../infrastructure/zlink/actors/PlayerActor.kt` | actor-adapter | done | bound session push를 감싼다. |
| `.NET: Server/Play/Infrastructure/ZLink/Actors/PlayerActorFactory.cs` | `Server/Play/src/main/kotlin/.../infrastructure/zlink/actors/PlayerActorFactory.kt` | actor-adapter | done | player actor 생성 책임. |
| `.NET: Server/Play/Infrastructure/ZLink/Handlers/AllocateBingoRoomHandler.cs` | `Server/Play/src/main/kotlin/.../infrastructure/zlink/handlers/AllocateBingoRoomHandler.kt` | channel-handler | done | Api의 room allocation 요청을 application use case로 연결한다. |
| `.NET: player actor 준비와 session bind` | `Server/Session/src/main/kotlin/.../AuthenticateSessionHandler.kt`; `Server/Play/src/main/kotlin/.../spots/entryspot/BingoEntrySpot.kt` | actor-lifecycle | done | Session이 global `getOrCreate` 결과의 exact `ActorRef`를 bind하고 Entry Spot lifecycle이 create request를 적용한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/BingoEntrySpot.cs` | `Server/Play/src/main/kotlin/.../spots/entryspot/BingoEntrySpot.kt` | spot-adapter | done | actor admission, room join, observer join 흐름. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/MatchBingoActorHandler.cs` | `Server/Play/src/main/kotlin/.../spots/entryspot/handlers/MatchBingoActorHandler.kt` | spot-handler | done | actor matching request 처리. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/ObserveBingoEventsHandler.cs` | `Server/Play/src/main/kotlin/.../spots/entryspot/handlers/ObserveBingoEventsHandler.kt` | spot-handler | done | observer용 local room join 처리. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/BingoRoom.cs` | `Server/Play/src/main/kotlin/.../spots/bingoroomspot/BingoRoomSpot.kt` | spot-adapter | done | room Spot lifecycle, domain 호출, push, reward pub/sub 수신을 맡는다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/BingoRoomSettingsPayloadMapper.cs` | `Server/Play/src/main/kotlin/.../domain/bingo/BingoRoomModels.kt` | spot-payload | done | Kotlin은 settings model 생성으로 payload mapping을 표현한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/BingoRewardAcquiredEventHandler.cs` | `Server/Play/src/main/kotlin/.../spots/bingoroomspot/handlers/BingoRewardAcquiredEventHandler.kt` | pubsub-handler | done | reward event를 observer bound session으로 push한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/BingoRoomDrawTimerHandler.cs` | `Server/Play/src/main/kotlin/.../spots/bingoroomspot/handlers/BingoRoomTimerHandler.kt` | timer-handler | done | server timer draw 진행. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/StopObservingBingoEventsHandler.cs` | `Server/Play/src/main/kotlin/.../spots/bingoroomspot/handlers/StopObservingBingoEventsHandler.kt` | spot-handler | done | observer actor를 observer room에서 제거한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/SubmitBingoCardHandler.cs` | `Server/Play/src/main/kotlin/.../spots/bingoroomspot/handlers/SubmitBingoCardHandler.kt` | spot-handler | done | card 제출 request를 room domain operation으로 연결한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Notifications/BingoNotificationPublisher.cs` | `Server/Play/src/main/kotlin/.../spots/bingoroomspot/BingoRoomSpot.kt` | notification | done | Kotlin은 별도 publisher 계층 없이 Spot adapter 안에서 actor public push method를 호출한다. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Notifications/BingoRoomEvent.cs` | `Server/Play/src/main/kotlin/.../domain/bingo/BingoRoomModels.kt` | domain-event | done | room event model. |
| `.NET: Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Notifications/BingoRoomEventMapper.cs` | `Server/Play/src/main/kotlin/.../spots/bingoroomspot/BingoRoomSpot.kt` | notification | done | domain state를 notify payload로 변환한다. |
| `.NET: Shared/Bingo.Shared.csproj` | `Shared/build.gradle.kts` | build | done | Shared project. |
| `.NET: Shared/Contracts/SampleConstants.cs` | `Server/Configuration/.../SampleNames.kt`, `Client/.../SampleNames.kt` | shared-config | done | Kotlin은 client/server 설정 object로 나누어 사용한다. |
| `.NET: Shared/Contracts/bingo_messages.proto` | `Shared/src/main/proto/bingo_messages.proto`, `Shared/src/main/kotlin/.../shared/contracts/Messages.kt` | shared-contract | done | Kotlin은 checked-in Protobuf schema에서 `Messages` generated class를 만들고, `Messages.kt`는 생성 message typealias와 builder wrapper만 제공한다. |

## 공통 요구 대응

| 기준 | Kotlin 대응 | 분류 | 상태 | 비고 |
|------|-------------|------|------|------|
| common: client는 Session stream endpoint 하나만 알고 연결 | `Client/Program.kt` | validation | done | player-1은 Session A, player-2와 observer는 Session B stream만 직접 연결한다. |
| common: Api 2개, Session 2개, Play 2개 실행 | `run_sample.sh`, `run_sample.ps1` | runner | done | 실제 process 경계로 role을 실행하고 공통 위치 store로 서로를 찾는다. |
| common: actor/session binding | `Session/.../BingoSession.kt`, `AuthenticateSessionHandler.kt`, `PlayerActor.kt` | runtime-flow | done | 인증 후 actor를 만들고 bound session push 경로를 사용한다. |
| common: Entry Spot에서 room Spot join | `BingoEntrySpot.kt`, `MatchBingoActorHandler.kt` | spot-flow | done | actor matching 후 room Spot으로 join한다. |
| common: global room Spot join 검증 | `BingoClientScenario.kt` | validation | done | client는 Actor·room의 owner NodeRid를 비교하지 않고 같은 `RoomId`의 상태와 notify 결과를 확인한다. |
| common: Spot pub/sub reward fan-out | `BingoRoomSpot.kt`, `BingoRewardAcquiredEventHandler.kt` | pubsub | done | owner room event를 observer용 local room에서 받아 push한다. |
| common: Redis-backed match queue | `RedisBingoMatchQueue.kt` | external-adapter | done | Redis를 application port 뒤에 둔다. |
| common: Redis-backed location store | `SampleLocationStore.kt`, role application classes | runtime-config | done | Api, Session, Play role은 `ZLinkRedisLocationStore` bean을 등록하고 framework가 public Spring configurer 경로로 사용한다. |
| common: 실행별 전용 Redis 사용 | `run_sample.sh`, `run_sample.ps1` | runner | done | runner가 pinned image로 전용 Docker Redis를 만들고 외부 endpoint를 재사용하지 않는다. |
| common: Docker Redis는 runner 책임 | `run_sample.sh`, `run_sample.ps1` | runner | done | 애플리케이션은 runner가 만든 endpoint만 받고, runner는 자신이 만든 container id만 정리한다. |
| common: 실행별 Redis key prefix 사용 | `run_sample.sh`, `run_sample.ps1` | runner | done | `BINGO_REDIS_KEY_PREFIX`가 없으면 실행별 prefix를 만든다. |
| common: Protobuf schema와 생성 message 사용 | `Shared/src/main/proto/bingo_messages.proto`, `Shared/.../Messages.kt` | shared-contract | done | `com.google.protobuf` Gradle plugin으로 schema를 generate하고 Kotlin public sample code는 generated `Messages.*` payload를 typealias로 사용한다. |
| common: stream/channel/actor/room Spot payload는 Protobuf codec 사용 | `Program.kt`, role application classes, `Shared/src/main/proto/bingo_messages.proto` | codec | done | Protobuf codec을 등록한 stream/channel/actor/Spot 경로에서 generated message payload를 사용한다. |
| common: connector wait API로 push 대기 | `BingoClientScenario.kt` | validation | done | `waitFor(...).submit(...)`과 `await(...)`를 사용한다. |
| common: inbound observer는 connect 전에 등록 | `Client/Program.kt` | validation | done | connector 생성 직후 inbound observer를 등록하고 이후 scenario에서 connect한다. |
| common: inbound observer 로그 확인 | `run_sample.sh`, `run_sample.ps1` | runner | done | `stream-inbound sample=Bingo`와 Notify 수신 marker를 확인한다. |
| common: sample-local polling으로 push 대기를 숨기지 않음 | `BingoClientScenario.kt` | validation | done | push 대기는 scenario 코드에 직접 드러난다. |
| common: Domain은 framework 타입을 모름 | `Server/Play/.../domain/bingo` | design | done | domain package는 framework runtime 타입을 모르며, 외부로 내보내는 room snapshot만 shared protobuf contract message로 만든다. |
| common: Redis client dependency는 adapter 안에 둠 | `RedisBingoMatchQueue.kt` | design | done | handler, actor, Spot, Domain에 Redis client 타입을 노출하지 않는다. |

## 남은 gap

현재 Kotlin `Bingo` 샘플에는 남은 gap이 없다. Shared 계약은 checked-in `bingo_messages.proto`에서 생성한 protobuf message를 사용하고, release gate는 schema와 generated-message wrapper가 함께 유지되는지 확인한다.
