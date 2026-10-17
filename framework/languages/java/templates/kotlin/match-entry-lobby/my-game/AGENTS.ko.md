# MyGame 개발 가이드

## 프로젝트 개요

이 Kotlin 프로젝트(Java 25 toolchain 위의 Kotlin 2.3.21, Gradle 빌드)는 액션/FPS 로비와 서버가 판정하는 2인 방을 제공하는 예제다. 연결을 처리하는 Session gateway, Match host 둘(`matchA`·`matchB`)과 위치·이전 상태를 저장하는 Redis를 실행한다. `build.gradle.kts`는 ZLink Framework package를 `0.30.1`(`frameworkVersion`)로, ZLink binding을 `1.19.1`(`bindingVersion`)으로 고정한다.

게임 기능은 아래의 프로젝트 구성과 확장 절차에 따라 추가한다. 실행 환경·배포·공개 package의 알려진 문제는 [README](README.md)가 설명한다. 초기 node 구성과 scenario 목록은 [zlink.structure.json](zlink.structure.json)에 있으며, 실행 중 객체의 위치는 Framework와 Redis가 관리한다.

> **문서와 package 버전.** [ZLink 문서](https://docs.zlink.systems/ko/), [Kotlin 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/), [API reference](https://docs.zlink.systems/ko/kotlin/reference/)와 [에이전트 색인](https://docs.zlink.systems/llms.txt)이 공개 문서의 진입점이다. 현재 가이드가 더 새 버전을 설명하면 `build.gradle.kts`에 고정된 package의 API와 대조한다.

## 개발 명령

기본 실행은 Docker에서 node와 client를 모두 실행하므로 호스트 JDK가 필요 없다. 이 디렉터리에서 다음 명령을 실행한다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario hit,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash도 같은 절차를 실행한다.

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario hit,reconnect,update
./run-e2e.sh --check-structure
```

로컬 process 디버깅은 JDK 25를 설치한 뒤 `--local` 또는 `-Local`로 실행한다. 로컬 빌드가 있으면 `--no-build` 또는 `-NoBuild`를 추가하며, 이 옵션은 `--local`/`-Local`과 함께만 쓴다. 시나리오 순서와 assertion은 유지하며 최초 실패를 README의 Known issues와 대조한다. 빌드와 e2e는 Windows와 Linux 모두에서 실행한다. runner는 node를 시작하기 전에 구조를 검사하며, 실행마다 고유한 Redis key prefix, Compose project, OS가 배정한 포트를 쓰므로 runner를 고칠 때도 이 격리를 유지한다.

## 프로젝트 구성과 확장 위치

### Session gateway

`server/session/src/main/kotlin/MyGame/session/Program.kt`는 STREAM node와 RouteMesh client를 등록하고, `GameSession`(같은 파일)이 수신 packet을 처리한다. 연결별 통신 객체가 Session이며, Framework는 Session을 플레이어 상태 객체와 연결하는 binding을 제공한다. `GameSession`은 binding에서 `SessionInfoRequest`에 응답하고, `QueueRequest`를 받으면 Player Actor를 구하거나 만들어 binding하며, 그 밖의 packet은 binding된 Actor로 relay한다. 인증은 binding 전에 추가하고 typed packet relay는 유지하여 플레이어에게 응답할 연결을 Framework가 확인하게 한다.

### Matchmaking과 방

ID로 찾는 게임 상태 객체가 Actor이며, `server/match/src/main/kotlin/MyGame/match/Player.kt`의 `Player`는 application 상태를 갖지 않는다. Actor를 받아 공통 게임 규칙을 실행하는 상태 객체가 Spot이다. `server/match/src/main/kotlin/MyGame/match/Spots.kt`의 고정 `Matchmaking` User Spot이 `MatchmakingState`로 대기열과 방 할당을 관리하고, 동적 `Room` User Spot이 `RoomState`로 위치·HP·행동 sequence·승자를 관리한다. 순수 규칙(`MatchmakingState`, `RoomState`, 벽·사거리 검사)은 `server/match/src/main/kotlin/MyGame/match/Domain.kt`에 있다.

`server/match/src/main/kotlin/MyGame/match/Program.kt`가 handler, Entry Spot, `Room`·`Matchmaking` factory(`SPOT_WIDE`, `stableTypeLimit`), `Player` factory를 등록한다. `Player.kt`의 `Entry`와 `EnterQueue`가 플레이어 진입을 처리하고, `server/match/src/main/kotlin/MyGame/match/Handlers.kt`의 `Actions` 클래스가 플레이어 요청과 push를 Spot에 연결한다. 설정에서 `bootstrap`이 true인 node(`matchA`)가 시작할 때 고정 Matchmaking Spot을 만들며, 다른 match host를 시작하기 전에 그 node의 `READY`를 기다린다. node 구성을 바꾸면 실제 등록과 `server/config/`, descriptor를 함께 맞추어 구조 검사가 같은 구성을 검사하게 한다.

### 공통 계약과 hosting

`server/shared/src/main/kotlin/MyGame/Contracts.kt`는 process 간 이름·게임 상수·메시지를 정의한다. `Hosting.kt`는 `NodeConfig`(process 설정), Redis store, hosting과 진단을 설정하는 `configure`, node를 시작하고 stdin 제어 명령을 처리하는 `runNode`를 정의하고, `Control.kt`는 제어·준비 문자열을 정의한다. `Names`와 `Game`은 이름과 게임 상수를, `Rejection`은 거부 사유를 가지고, `Transfer.kt`는 relocation adapter용 `ensureActive`를 정의한다. hosting과 제어 protocol은 role마다 복제하지 않고 이곳에 둔다. 여러 process가 사용하는 계약은 `Contracts.kt`에 추가하며, 대기열과 방 상태는 `Matchmaking`과 `Room`에 유지하여 서로 다른 process가 같은 값을 따로 변경하지 않게 한다.

### E2E client

`e2e/src/main/kotlin/MyGame/e2e/Client.kt`는 공개 stream connector로 게임에 접속하고 `Scenarios.kt`는 client가 관찰할 결과를 검사한다. `Nodes.kt`는 실행별 process와 Compose project를 관리하고, `Structure.kt`는 구성을 검사하며, `Program.kt`는 시나리오를 고르고 종료 코드를 정한다(시나리오나 정리가 실패하면 종료 코드는 1이다). `DockerRuntime.kt`는 Docker 모드에서 node를 container로 실행하고 `Cleanup.kt`는 정리 실패를 모은다. 새 게임 동작은 client가 받은 결과로 검증한다. `Domain.kt`의 순수 규칙에는 `server/match/src/test/kotlin/MyGame/match/DomainTest.kt`의 unit test가 있으며, `server:match` 모듈의 Gradle `test` task로 실행한다.

## 프로젝트의 ZLink 개념

### RouteMesh와 channel

process 간 메시지를 전달할 node들을 연결하는 구성이 RouteMesh이며, 이 프로젝트에서는 `Names.MESH`(`my-game-arena`)를 사용한다. 같은 목적의 메시지를 이름으로 구분하는 channel은 이 프로젝트에 선언된 것이 없다(`zlink.structure.json`의 `channels`가 비어 있다). 등록과 호출은 [Channel messaging](https://docs.zlink.systems/ko/kotlin/guide/server/20-channel-messaging/)에서 설명한다.

### Spot과 Actor

`Entry`는 Actor 진입을 처리하고, `Matchmaking`은 대기 중인 플레이어를 짝짓는 고정 User Spot이며, `Room`은 짝마다 요청에 따라 만드는 User Spot이다. [Spot 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/21-spot/)는 이 객체들의 등록과 호출을 설명한다. `Player`는 플레이어 ID로 호출하며 application 상태를 갖지 않으며, 생성과 호출은 [Actor 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/22-actor/)에서 설명한다.

### STREAM과 Session

client 연결의 packet을 처리하는 [STREAM](https://docs.zlink.systems/ko/kotlin/guide/server/23-stream/)에서 `GameSession`은 Player Actor를 구해 binding한 뒤 원래 typed packet을 전달한다. Actor handler는 binding된 Session을 통해 client에 메시지를 보낸다. 연결과 Actor 사이의 관계는 [Session and Actor](https://docs.zlink.systems/ko/kotlin/guide/server/24-actor-session/)가 설명한다.

### Location Store

객체 ID에 해당하는 현재 실행 위치를 저장하는 저장소가 Location Store이며, 이 프로젝트는 Redis 구현(`Hosting.kt`의 `ZLinkRedisLocationStore`)을 사용한다. application은 node 주소 대신 Actor와 Spot의 ID로 호출하므로 위치가 바뀌어도 호출 대상을 같은 ID로 지정한다. 조회 방식은 [Location 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/25-location/)가 설명한다.

### Actor membership

Actor가 소속 Spot을 바꾸는 동작은 membership 변경이다. 대상 Spot이 참가 요청의 수락 여부를 정하는 단계가 admission이다. `EnterQueue.queue`는 `Matchmaking`으로의 참가를 `joinSpot(...).defer()`로 지연 요청하고, `Matchmaking.match`가 `RoomAssignment`를 보내면 `Actions.assign`이 `Room` 참가를 지연 요청한다. `Room.onActorJoinSuspending`이 수락 여부를 정하고 `Player.onJoinCompletedSuspending`은 거부를 client에 전달한다. 참가 수락과 완료의 순서는 [Actor membership](https://docs.zlink.systems/ko/kotlin/guide/server/35-actor-membership/)이 설명한다.

### Host relocation과 drain

객체의 실행 host를 변경하는 동작은 relocation이며, host를 종료할 수 있도록 workload를 다른 host로 이동시키는 절차는 drain이다. application adapter는 게임 상태를 저장하고 복원하며 Framework는 workload와 route를 이전한다. `Hosting.kt`의 `runNode`는 `drain`을 읽으면 host relocation을 요청하고 관찰한 뒤 `SAFE_TO_SHUTDOWN`을 알린다. `MatchmakingTransfer`와 `RoomTransfer`는 Spot 상태를 JSON으로 저장하고 `PlayerTransfer`는 아무것도 저장하지 않는다. 절차는 [Relocation 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/37-relocation/)가 설명한다.

## 요청 흐름

예시 게임의 요청은 다음 경로로 처리된다. 기능을 더하거나 옮기기 전에 이 경로를 따라 코드를 읽으면 바꿀 위치를 빠르게 찾을 수 있다.

1. client가 Session node에 STREAM으로 연결하고 첫 패킷 `QueueRequest`(플레이어 ID와 mode)를 보낸다. `GameSession`은 `playerId`로 Player Actor를 `getOrCreate`한 뒤 `bindOrGet`으로 Session에 binding하고 원래 패킷을 relay한다.
2. 새 Actor는 Entry Spot `Entry`에서 시작한다. `EnterQueue.queue`가 identity를 확인하고 `joinSpot(Names.MATCHMAKING_ID, request).defer()`를 요청한다.
3. `Matchmaking.onActorJoinSuspending`이 요청을 대기열에 넣는다. 참가 뒤 `Matchmaking.match`가 같은 mode의 두 플레이어를 짝짓고 `getOrCreate`로 다음 방을 만든 뒤 각 플레이어에게 `RoomAssignment`를 보낸다.
4. `Actions.assign`이 할당을 받아 `joinSpot(roomId, RoomAdmission).defer()`를 요청한다. `Room.onActorJoinSuspending`이 플레이어를 받아들이고, `Room.onJoinedActorSuspending`이 `MatchFound`를 보내고 `RoomView`를 broadcast한다.
5. 방 안에서 `move`와 `shoot` 요청은 `Room`에 도달하며 `RoomState` 규칙이 적용된다. 거부(`ActionRejected`, `ShotRejected`)는 요청자의 binding된 Session으로 보내고, 수락된 행동은 `RoomView`를 broadcast하며 승자는 `MatchEnded`로 알린다. 받는 쪽 `Actions` handler(`viewPush`, `foundPush`, `endedPush`)가 push를 각 Actor의 binding된 Session으로 전달한다.
6. 재접속한 client는 새 Session에서 `QueueRequest`를 보낸다. 같은 Actor가 binding되고 `Actions.reconnect`가 identity를 확인한 뒤 방 view를 다시 보낸다.
7. node를 교체할 때 `runNode`의 `drain`이 relocation을 요청한다. Framework는 `MatchmakingTransfer`·`RoomTransfer`·`PlayerTransfer`의 `capture`로 application 상태를 받아 다른 node에서 `restore`한다.

## 이 샘플을 보며 개발하기

이 샘플은 기능들이 함께 동작하는 방식을 보여 주므로, 자기 게임으로 바꾸지 않고 읽고 실행하는 용도로 쓴다. 자기 프로젝트는 Kotlin 기본 셋업에서 시작한다. [ZLink Start](https://zlink.systems/start)의 Initializer에서 Kotlin을 고르거나 ZLink 저장소의 `framework/languages/java/templates/kotlin/starter/my-game`을 사용한다. 그다음 필요한 기능을 이 샘플에서 하나씩 옮긴다. 구조 선택은 기본 셋업의 `AGENTS.md`가 설명하며, 이 절은 `match-entry-lobby`만 다룬다. 이 샘플은 바꾸지 않고 프로젝트 옆에 참고용으로 둔다.

### 샘플 먼저 실행하기

옮기기 전에 [개발 명령](#개발-명령)으로 빌드와 e2e를 한 번 실행해 받은 샘플이 동작하는지 확인하고 동작을 본다. 이 결과가 이후 비교의 기준이 된다. 실패하면 먼저 [README](README.md)의 Known issues와 대조한다.

### 옮길 기능

기능 이름은 기본 셋업 `AGENTS.md`의 기능 표와 같다. 경로는 이 디렉터리를 기준으로 한다. 기본 셋업에는 Session node 하나와 `connect` 시나리오만 있으므로, 마지막 열은 기능마다 기본 셋업에서 함께 늘릴 것이다.

| 기능 | 이 샘플의 파일 | 옮길 것 | 기본 셋업에서 함께 늘릴 것 |
| --- | --- | --- | --- |
| Typed 메시지 | `server/shared/src/main/kotlin/MyGame/Contracts.kt` | 사용할 기능의 클래스: `QueueRequest`, `RoomAdmission`, `RoomAssignment`, `MatchFound`, `MoveRequest`, `ShootRequest`, `RoomViewRequest`, `PlayerView`, `RoomView`, `ShotRejected`, `ActionRejected`, `MatchEnded`. `SessionInfoRequest`·`SessionInfo`는 client가 필요로 할 때만 옮긴다 | `Names`·`Game`·`Rejection`의 mesh·타입 이름과 상수. node `Roles`와 제어 문자열은 기본 셋업에 이미 있다 |
| Actor | `server/match/src/main/kotlin/MyGame/match/Player.kt` | `onJoinCompletedSuspending`(거부를 `ActionRejected`로 전달)을 가진 `Player`, `PlayerFactory` | 새 `server/match/` Gradle 모듈(`settings.gradle.kts`에 포함)과 그 `Program.kt`, `server/config/match-a.json`, `server/config/match-b.json`, `e2e/.../Nodes.kt`의 시작 처리, `Roles` 항목. `Program.kt`의 `addActorFactory` 등록. `zlink.structure.json`의 node 선언과 이에 맞춘 `Structure.kt` 검사(기본 셋업의 검사는 `session` 역할만 받는다) |
| Spot | `server/match/src/main/kotlin/MyGame/match/Spots.kt`의 `Room`·`Matchmaking`, `server/match/src/main/kotlin/MyGame/match/Domain.kt` | `RoomState`(admission, 이동, 명중, 승자)를 가진 `Room`, 로비가 필요하면 `MatchmakingState`를 가진 `Matchmaking`, `Domain.kt`의 순수 규칙 | `Program.kt`의 `addSpotFactory` 등록(`SPOT_WIDE`, `stableTypeLimit`)과 `NodeConfig`의 `roomCapacity`. 규칙의 unit test. `zlink.structure.json`의 `spots` 선언과 이를 등록과 비교하는 `Structure.kt` 검사(기본 셋업의 검사는 Spot이 없다고 단정한다) |
| Entry·첫 입장 | `server/match/src/main/kotlin/MyGame/match/Player.kt`의 `Entry`·`EnterQueue`, `server/match/src/main/kotlin/MyGame/match/Handlers.kt`의 `Actions.reconnect` | `EnterQueue.queue`: identity 확인과 `Matchmaking`으로의 deferred join, `Actions.reconnect`의 identity 확인과 view 재전송 | `addEntrySpot` 등록과 `entry` 선언. 첫 매치를 보는 `hit` 시나리오와 재접속 뒤 상태를 비교하는 `reconnect` 시나리오 |
| Session binding | `server/session/src/main/kotlin/MyGame/session/Program.kt` (`GameSession`) | 첫 패킷 분기: Actor 조회·생성, `bindOrGet`, 원래 typed 패킷 relay | `EnterQueue.queue`와 같은 첫 패킷 타입. 기본 셋업의 `PlayerSession.kt`에 옮긴다 |
| Host relocation | `server/match/src/main/kotlin/MyGame/match/Spots.kt`의 `MatchmakingTransfer`·`RoomTransfer`, `server/match/src/main/kotlin/MyGame/match/Player.kt`의 `PlayerTransfer`, `server/shared/src/main/kotlin/MyGame/Transfer.kt` | JSON으로 저장하는 `MatchmakingState`·`RoomState`와 `capture`·`restore`, 비어 있는 Player adapter, `ensureActive` | `preserveStateWith` 등록. 서버 교체 뒤에도 연결·방 상태가 유지되고 대체 node가 새 방을 받는지 판정하는 `update` 시나리오 |
| 동적 던전 | `server/match/src/main/kotlin/MyGame/match/Spots.kt` (`Matchmaking.match`), `server/match/src/main/kotlin/MyGame/match/Domain.kt` (`MatchmakingState.allocateRoom`) | 동적 방 생성: 방 ID 할당, `getOrCreate(roomId, Names.ROOM)`, `RoomAssignment` push | `zlink.structure.json`의 동적 `room` Spot. 고정 `matchmaking` Spot, `NodeConfig`의 `bootstrap`, `Hosting.kt`의 `getOrCreate` 호출 |

### 옮기는 순서와 규칙

기능은 표의 행 하나씩 옮기고, 하나를 옮길 때마다 빌드, 구조 검사, 관련 e2e 시나리오를 실행한다. 시나리오 ID는 `Contracts.kt`의 `Names.SCENARIOS`, `e2e/src/main/kotlin/MyGame/e2e/Scenarios.kt`의 `scenario` 함수, `zlink.structure.json`의 `run.scenarios`, `run.commands`의 시나리오 선택 예시에 함께 추가한다. 판정은 실제로 받은 응답과 이벤트로 하고, 고정 대기나 기한 확대로 통과시키지 않는다.

메시지 contract부터 옮긴다. `Contracts.kt`에 typed 클래스를 먼저 정의한 뒤, 컴파일 오류가 나는 handler를 하나씩 새 메시지로 옮긴다. 첫 패킷(`QueueRequest`)을 바꾸면 `GameSession`의 첫 패킷 분기, `EnterQueue.queue`, `Actions.reconnect`가 같은 타입을 받도록 함께 고친다.

상태는 그 상태를 결정하는 객체에 둔다. 대기열과 방 할당은 `Matchmaking`, 위치·HP·행동 sequence·승자는 `Room`에 두며 `Player`는 상태를 갖지 않는다. `MatchmakingState`나 `RoomState` 필드를 바꿀 때는 같은 객체의 `capture`·`restore`를 함께 고친다. 빠뜨린 필드는 relocation 뒤 사라지고, `update` 시나리오가 그 손실을 잡는다.

게임 규칙은 Framework 호출이 없는 `Domain.kt`에 두고 unit test로 검증하며, `Spots.kt`의 callback에서 호출한다. e2e runner는 `roomCapacity`를 상수로 정해 `update`가 room host를 drain한 뒤 대체 node만 다음 방을 받게 한다. 방을 만드는 시나리오를 추가할 때도 이 구성을 유지한다.

역할 이름을 이 샘플과 다르게 정하려면 타입 이름, `Names`, `server/config/`, `zlink.structure.json`을 한 번에 바꾼다. 이름 변경은 기능을 옮기는 변경과 나눠서 진행하면 실패 원인을 찾기 쉽다.

옮긴 뒤 `update`가 실패하면 snapshot에 빠진 상태가 있는지 먼저 확인한다. 그다음 [디버깅](#디버깅)의 message flow로 첫 실패 단계를 찾는다.

## 개발 시 주의점

### 상태와 메시지 계약

의존성은 `build.gradle.kts`에 고정한 Maven Central의 공개 package를 사용하며, 프로젝트 의존은 이 프로젝트의 Gradle 모듈 사이에 둔다. typed 메시지는 기본 Framework JSON serializer로 처리하여 process마다 같은 계약을 사용한다. relocation adapter(`*Transfer` 클래스)가 직접 직렬화하는 대상은 application 상태로 한정하여 Framework의 실행 상태와 게임 상태의 관리 책임을 분리한다. route·lease·membership·completion·queue·execution gate는 Framework가 관리하므로 application 상태에 같은 결정을 복제하지 않는다.

플레이어 HP 같은 게임 사실은 그것을 변경하는 객체 한 곳(`Room`)에서 관리한다. 매칭 대기열은 `Matchmaking`에만 둔다. Actor handler 안의 응답은 그 Actor의 binding된 Session으로 보내고, 그 밖의 push는 공개 Actor client로 Actor를 플레이어 ID로 지정하여 보내며 받는 Actor handler가 binding된 Session으로 전달한다. Spot은 나중의 전달을 위해 Actor 참조를 보관하지 않는다. host relocation이 이전 대상에서 `onJoinedActor`를 다시 호출하지 않기 때문이다. Actor identity는 Session binding이 소유하므로 게임 메시지에 중복해서 담지 않는다(`SessionInfo`는 현재 binding을 읽는다). Session은 binding 뒤 packet을 전달하고, 게임 판정은 해당 상태를 관리하는 Actor나 Spot에서 수행한다.

### 비동기 실행과 lifecycle

공개 호출은 템플릿처럼 `await()`로 기다리며, handler와 callback에서 `.join()`, `.get()`, `runBlocking`, `Thread.sleep` 같은 blocking 호출을 쓰지 않는다. `Room`과 `Matchmaking`은 `SPOT_WIDE`로 등록되어 같은 Spot의 Spot·멤버 Actor handler와 lifecycle callback이 공통 execution gate를 사용한다. 같은 상태를 별도 lock이나 queue로 다시 보호하지 않는다. await 중에도 turn을 유지할 수 있으므로 같은 Spot에 대한 request처럼 그 turn이 끝나야 실행되는 작업을 기다리지 않으며, 긴 작업은 handler 밖에서 처리한다. 실행 경계는 [Execution model](https://docs.zlink.systems/ko/kotlin/guide/server/32-execution-model/)이 설명한다.

타입 등록과 객체별 callback은 각각 정해진 lifecycle 단계에 둔다. `Matchmaking`은 bootstrap에서 만드는 고정 User Spot이고 `Room`은 `Matchmaking.match`가 `getOrCreate`로 요청에 따라 만드는 User Spot이며, 생성과 초기화의 차이는 [Activation and lifetime](https://docs.zlink.systems/ko/kotlin/guide/server/34-activation-lifetime/)이 설명한다. membership 변경은 `joinSpot(...).defer()`와 공개 완료 callback(`onJoinCompletedSuspending`)을 사용하고 host 종료는 relocation과 runtime 관찰(`drain` 다음 `SAFE_TO_SHUTDOWN`)로 확인하여 요청 수락을 완료로 오인하지 않는다. `Control.kt`는 safe-shutdown 대기(`SAFE_DEADLINE`)를 Message Follow 시간(`MESSAGE_FOLLOW`)과 host deadline(`HOST_DEADLINE`)에서 유도하므로 이 값들은 그곳에서만 바꾼다.

### 재접속과 실패 처리

reconnect에서는 같은 플레이어 ID를 사용하고 새 Session을 같은 Actor에 binding하여 기존 플레이어 상태를 이어서 사용한다. 현재 위치는 Framework가 관리하므로 application route cache를 추가하거나 reconnect 때 플레이어 상태를 초기화하지 않는다. 처리 용량 때문에 호출을 수락하지 못하거나 대기시키는 동작이 backpressure이며, [Backpressure 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/33-backpressure/)의 공개 결과를 관찰한다.

Framework 실패는 실제 reply·event·lifecycle 결과를 근거로 조사한다. Internal API·reflection·raw frame·사설 codec·추가 poller·고정 sleep·transport 재시도·deadline 확대·assertion 완화는 실패한 공개 동작을 검증하지 못하므로 해결 방법으로 추가하지 않는다. 코드 주석은 영어로 작성하고 실행·빌드 산출물인 `.logs/`, `build/`, `.gradle/`, `.kotlin/`과 license 파일(`zlink.license`)은 commit에서 제외한다. 프로젝트에는 formatter 설정이 없으므로 Kotlin 소스를 정리할 때는 이 디렉터리에서만 ktfmt `--kotlinlang-style`을 사용한다.

## 디버깅

### 실행별 로그

첫 실패 실행을 보존하고 로그를 읽은 뒤 재실행 여부를 결정한다. 간헐 실패도 실제로 관찰한 결함이며 이후 통과로 첫 실패를 제외하지 않는다. runner는 시작할 때 `Logs: <path>`를 출력한다. `.logs/run-<id>/`에는 `Nodes.kt`가 node 시작 시 작성한 실제 설정(`session.json`, `matchA.json`, `matchB.json`)과 node 시작마다 만드는 `<node>-<uuid>/` 디렉터리가 있으며, 그 안의 `<node>-stdout.log`는 준비·제어 줄을, `<node>-stderr.log`는 Framework 로그와 message flow를 기록한다. 교체한 node는 같은 설정 파일을 쓰고 새 디렉터리에서 시작한다.

로컬 모드에서는 `compose.log`가 Compose 명령 출력을, `.logs/build.log`가 로컬 빌드 출력을 기록한다. Docker 모드의 빌드, Redis, 바깥 정리 로그는 `.logs/docker-<project>/`(`build.log`, `redis-start.log`, `redis.log`, `cleanup.log`)에 있다.

### 진단 수준

`server/shared/src/main/kotlin/MyGame/Hosting.kt`의 `configure`는 메시지 수신부터 처리 완료까지의 단계를 기록하는 message flow를 다음 설정으로 활성화한다.

```kotlin
options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL)
```

`NORMAL`은 receive·dispatch·completion 단계와 오류를 기록하고, `ERRORS`는 오류만, `OFF`는 기록하지 않는다. 시간 상세가 필요하면 `DETAILED`로 바꾸며, 크기 기록에는 같은 dispatch 옵션에 `includeMessageSizes(true)`도 호출한다. 상세 기록에는 비용이 추가되므로 필요한 조사 범위에서 사용한다.

`NodeConfig`는 diagnostics JSON key를 정의하지 않으므로 시작 설정은 `configure` 호출을 수정하고 다시 빌드·시작하여 적용한다. 실행 중에는 `ZLinkFrameworkRuntime.setMessageFlowModeAsync`로 mode를 바꿀 수 있지만 이 프로젝트의 stdin 제어 명령에는 연결되어 있지 않다. 진단 항목과 설정은 [Monitoring](https://docs.zlink.systems/ko/kotlin/guide/server/26-monitoring/)과 [Observability diagnostics](https://docs.zlink.systems/ko/kotlin/reference/08-observability-diagnostics/)가 설명한다.

### Message flow 조사

Framework 로그는 message flow를 포함해 stderr로 기록되므로(`server/shared/src/main/resources/logback.xml`이 appender를 설정한다) message flow는 `<node>-stderr.log`에서 확인하며 별도 flow 파일을 만들지 않는다. record의 `flow`는 node를 통과하는 메시지를 추적하는 값이고 `corr`는 request와 reply의 관계를 확인하는 값이다. flow record에는 `zlink flow:`가 있으며 실패는 `outcome=failed`, `error_type`, `error_message`로 표시된다.

실패 필드와 바로 앞 단계를 함께 읽고 category를 제외하지 않는다. 실행마다 flow ID가 달라지므로 통과·실패 실행은 논리적 작업, Actor ID와 단계 순서로 비교하여 처음 누락되거나 실패한 단계를 찾는다. startup이 message flow 기록 전에 실패했으면 시작 예외와 stack을 보존한다.

### 보조 trace와 정리

e2e runner의 STREAM connector 추가 trace는 `--local` 실행 전에 부모 process 환경에 환경 변수를 설정한다. Docker runner와 node는 호스트 환경 변수를 상속하지 않으므로 필요하면 Compose service나 node 생성에 진단 환경 값을 명시한다. PowerShell에서는 다음 환경 변수를 사용한다.

```powershell
$env:ZLINK_JAVA_STREAM_TRACE = "1"
```

이 값은 그 환경에서 시작하는 child process에 적용된다. Bash에서는 같은 변수를 다음과 같이 설정한다.

```bash
export ZLINK_JAVA_STREAM_TRACE=1
```

`ZLINK_JAVA_STREAM_TRACE`는 공개 `0.30.1` package에 있는 버전별 진단 switch이므로 routing API로 사용하지 않는다. runner는 자신이 만든 process와 Compose project를 정리하고 Docker socket 제어 경로를 유지하며, 정리 오류도 보존하고 다른 작업의 process나 container는 중단하지 않는다.

## ZLink 결함 보고

### 결함 판단

ZLink 결함은 공개 API를 문서대로 호출했는데 Framework가 문서와 다른 결과를 내는 경우다. 먼저 사용한 API를 위의 공개 가이드·샘플과 대조하여 application 사용 오류인지 확인한다. 사용 오류가 아니면 우회 코드를 추가하지 않고 issue로 보고하며, scenario assertion은 그대로 유지한다. 우회 코드는 결함을 감추고, 수정된 package로 올린 뒤에도 남아 동작을 바꾸기 때문이다.

### 기존 issue 확인

등록 전에 [ZLink issues](https://github.com/zlink-systems/zlink/issues?q=is%3Aissue)에서 오류 메시지·예외 타입·API 이름으로 열린 issue와 닫힌 issue를 함께 검색한다. 같은 결함이 있으면 새 issue를 만들지 않고 그 issue에 재현 환경과 로그를 덧붙인다. 닫힌 issue와 같은 결함이면 수정이 포함된 package 버전을 issue에서 확인하고, `build.gradle.kts`의 `frameworkVersion`을 올려 다시 실행한다.

### issue 본문

본문은 ZLink 저장소의 issue template 항목을 따른다. 다음 형식을 복사해 채운다.

```markdown
# Issue description
<scenario and the public operation that fails>

# Environment
* zlink version: <frameworkVersion and bindingVersion from build.gradle.kts>
* OS: <OS, JDK version>

# Minimal test code / Steps to reproduce the issue
1. <commands or minimal code using public APIs only>

# What's the actual result? (include assertion message & call stack if applicable)
<first failing node-log excerpt, exception and stack, flow/corr records>

# What's the expected result?
<observable result the guide or reference documents>
```

재현은 이 프로젝트 전체가 아니라 실패한 공개 호출만 남긴 최소 코드나 명령으로 줄인다. 로그는 첫 실패 구간과 그 앞 단계만 붙이고, 전체 로그가 필요하면 파일로 첨부한다. startup이 flow 기록 전에 실패했으면 그 사실을 적는다.

> **issue는 공개된다.** 등록 전에 로그와 설정에서 license key, token, 내부 host 이름, 개인 정보를 지운다.

### 등록 절차

코딩 에이전트는 issue 본문을 작성해 개발자에게 보여 주고, 개발자가 확인한 뒤에 등록한다. issue는 외부 저장소에 공개되는 글이므로 개발자의 확인 없이 등록하지 않는다. GitHub CLI를 쓰면 확인한 본문 파일로 다음과 같이 등록한다.

```bash
gh issue create --repo zlink-systems/zlink --title "<area>: <observed failure>" --body-file issue.md
```

제목에는 언어와 실패한 동작을 적는다(예: `framework(kotlin): Spot startup fails with empty SpotId`). 등록한 issue 번호는 README의 Known issues에 적어, 수정 package가 나오면 다시 검증할 위치를 남긴다.
