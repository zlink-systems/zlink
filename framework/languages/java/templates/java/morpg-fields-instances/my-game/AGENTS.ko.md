# MyGame 개발 가이드

## 프로젝트 개요

이 Java 25 프로젝트는 공용 Field가 심리스 월드처럼 여러 node에 나뉘고 파티가 Field에서 전용 던전으로 들어가는 MORPG 예제다. 연결을 처리하는 Session gateway, Field host A·B(Field 4개, host마다 2개), Dungeon을 요청에 따라 만드는 Instance host A·B와 위치·이동 상태를 저장하는 Redis를 실행한다. `build.gradle.kts`는 ZLink Framework를 `0.30.2`, binding을 `1.20.0`으로 고정한다.

게임 기능을 추가할 위치는 아래의 프로젝트 구성에, 이 샘플의 기능을 자기 프로젝트로 옮기는 절차는 "이 샘플을 보며 개발하기"에 있다. 실행 환경·배포·공개 package의 알려진 문제는 [README](README.md)가 설명한다. 초기 node 구성과 scenario 목록은 [zlink.structure.json](zlink.structure.json)에 있으며, 실행 중 객체의 위치는 Framework와 Redis가 관리한다.

> **문서와 package 버전.** [ZLink 문서](https://docs.zlink.systems/ko/), [Java 가이드](https://docs.zlink.systems/ko/java/guide/server/), [API reference](https://docs.zlink.systems/ko/java/reference/)와 [에이전트 색인](https://docs.zlink.systems/llms.txt)이 공개 문서의 진입점이다. 현재 가이드가 더 새 버전을 설명하면 `build.gradle.kts`에 고정된 버전의 API와 대조한다.

## 개발 명령

기본 실행은 Docker에서 node와 client를 모두 실행하므로 Docker(Compose v2)가 필요하고 호스트 JDK는 필요 없다. 이 디렉터리에서 다음 명령을 실행한다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario field,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash도 같은 절차를 실행한다.

```bash
./run-e2e.sh --build-only
./run-e2e.sh --check-domain
./run-e2e.sh
./run-e2e.sh --scenario field,dungeon,reconnect,update
./run-e2e.sh --check-structure
```

로컬 process 디버깅은 JDK 25를 설치한 뒤 `--local` 또는 `-Local`로 실행한다. 로컬 빌드가 있으면 `--no-build` 또는 `-NoBuild`를 추가한다. 시나리오 순서와 assertion은 유지하며 최초 실패를 README의 Known issues와 대조한다.

## 프로젝트 구성과 확장 위치

### Session gateway

`server/session/src/main/java/MyGame/session/GameSession.java`는 STREAM과 mesh listener를 등록하고 `server/session/src/main/java/MyGame/session/GameSession.java`는 연결 callback과 수신 packet 전달을 담당한다. 연결별 통신 객체가 Session이며, Framework는 Session을 플레이어 상태 객체와 연결하는 binding을 제공한다. `GameSession.onDispatch`는 첫 packet으로 `JoinWorldRequest`를 요구하고 `playerId`로 Player Actor를 구해 binding한 뒤 원래 packet을 relay한다. 인증은 binding 전에 추가하고 typed relay는 유지하여 플레이어에게 응답할 연결을 Framework가 확인하게 한다.

### Field와 플레이어

ID로 찾는 게임 상태 객체가 Actor다. `Player`는 좌표, 64-bit sequence와 Party Spot ID를 유지한다. `FieldPositionDomain`은 이동 권위, 도착 검사, 속도 예산과 남은 시간 스냅샷을 소유한다. `Field`는 가시성 정보와 Party Spot 요청을 관리하며 Field를 나가도 파티 구성원은 유지된다. 공용 application Entry Spot(`server/shared/src/main/java/MyGame/admission/Entry.java`, `server/shared/src/main/java/MyGame/admission/EnterWorld.java`)이 플레이어의 첫 진입을 좌표에 해당하는 Field로 보낸다.

`server/field/src/main/java/MyGame/field/Program.java`가 Entry Spot, Field·Party factory, Player factory, 공용 entry handler와 `border` channel을 등록한다. Party ID는 Party Spot ID이며 `PartySpot`이 파티 구성과 `Forming → Preparing → Entering → Active → Completed` 상태를 소유한다. `Actions.java`에는 이동·Party 요청·던전 입장·전달 handler가 있다. node 구성을 바꾸면 실제 등록과 `server/config/`, descriptor를 함께 맞추어 구조 검사가 같은 구성을 검사하게 한다.

### Dungeon

`server/instance/src/main/java/MyGame/instance/Program.java`가 Dungeon factory(`dungeonCapacity`)와 Entry Spot을 등록한다. `DungeonRun`은 owner Party Spot ID, Run ID, 구성원, phase와 stage를 저장한다. Dungeon ID는 서버 발급 `run-` ID에서 만들며 `Prepare.prepare`는 멱등 준비를 처리한다. 모든 구성원이 도착하면 Dungeon이 Party Spot에 `RunStarted`를 보내고 완료 때 `RunCompleted`를 보낸다. Dungeon 안의 `EnterDungeonRequest`는 현재 상태를 반환하고 마을 전용 Party 요청은 `run-in-progress`로 거부한다.

### 공통 계약과 hosting

`server/shared/src/main/java/MyGame/`는 typed message, RunPhase와 게임 상수를 정의한다. JSON 이름은 camelCase이고 enum은 이름 문자열이며 nullable 값은 `null`로 보낸다. 이동 sequence는 Java `long`을 쓴다. `NodeConfig.java`는 process 설정을, `Names.java`는 mesh와 타입 이름을 정의한다. `Hosting.java`와 `Stores.java`는 hosting·진단과 Redis store를 설정하고 `Control.java`는 제어 marker와 기한을 둔다. `Rules.java`는 월드 구조와 이동 상수를, `Player.java`·`PlayerFactory.java`·`PlayerTransfer.java`는 공용 Actor 상태를 관리한다. 계약은 공용 모듈에 두고 상태 판정은 해당 소유자에 둔다.

### E2E client

`Client.java`와 `Scenarios.java`는 공개 connector로 client가 관찰한 결과를 검사한다. `DomainChecks.java`에는 이동·PartyRun·DungeonRun과 JSON 계약 검사가 있으며 `./run-e2e.sh --check-domain`으로 따로 실행한다. `Nodes.java`는 node와 실행별 로그·자원을 관리하고 `Program.java`는 시나리오 또는 정리 실패를 종료 코드 1로 보고한다. `Structure.java`는 시작 전에 파일 구성을 검사한다. 시나리오 순서 `field,dungeon,reconnect,update`를 유지한다.

## 프로젝트의 ZLink 개념

### RouteMesh와 channel

process 간 메시지를 전달할 node들을 연결하는 구성이 RouteMesh이며, 이 프로젝트에서는 `Names.MESH`(`my-game-world`)를 사용한다. 같은 목적의 메시지를 이름으로 구분하는 channel 중 `border`는 Field 사이의 typed publication에 사용된다. 등록과 호출은 [Channel messaging](https://docs.zlink.systems/ko/java/guide/server/20-channel-messaging/)에서 설명한다.

### Spot과 Actor

`Entry`는 플레이어의 첫 진입을 받고 `Field`는 지리적 Field를, `Dungeon`은 파티 하나를 위해 만든 던전을 표현한다. [Spot 가이드](https://docs.zlink.systems/ko/java/guide/server/21-spot/)는 이 객체들의 등록과 호출을 설명한다. `Player`는 소속 Spot이 바뀌어도 플레이어 이동 상태를 유지하며, 생성과 호출은 [Actor 가이드](https://docs.zlink.systems/ko/java/guide/server/22-actor/)에서 설명한다.

### STREAM과 Session

client 연결의 packet을 처리하는 [STREAM](https://docs.zlink.systems/ko/java/guide/server/23-stream/)에서 `GameSession`은 Player Actor를 구해 binding한 뒤 원래 typed packet을 전달한다. Actor handler는 binding된 Session을 통해 client에 메시지를 보낸다. 연결과 Actor 사이의 관계는 [Session and Actor](https://docs.zlink.systems/ko/java/guide/server/24-actor-session/)가 설명한다.

### Location Store

객체 ID에 해당하는 현재 실행 위치를 저장하는 저장소가 Location Store이며, 이 프로젝트는 `server/shared/src/main/java/MyGame/Stores.java`가 만드는 Redis 구현을 사용하고 `docker-compose.yml`이 e2e 실행의 Redis service를 정의한다. application은 node 주소 대신 Actor와 Spot의 ID로 호출하므로 위치가 바뀌어도 호출 대상을 같은 ID로 지정한다. 조회 방식은 [Location 가이드](https://docs.zlink.systems/ko/java/guide/server/25-location/)가 설명한다.

### Actor membership

Actor가 소속 Spot을 바꾸는 동작은 membership 변경이다. 대상 Spot이 참가 요청의 수락 여부를 정하는 단계가 admission이다. `Player.move`는 좌표에 해당하는 Field 참가를, `Actions.travel`은 파티의 Dungeon 참가를 지연 처리로 요청하고 `Field.onActorJoin`과 `Dungeon.onActorJoin`이 수락 여부를 정하며 `Player.onJoinCompleted`는 완료 결과를 확인한다. 참가 수락과 완료의 순서는 [Actor membership](https://docs.zlink.systems/ko/java/guide/server/35-actor-membership/)이 설명한다.

### Host relocation과 drain

객체의 실행 host를 변경하는 동작은 relocation이며, host를 종료할 수 있도록 workload를 다른 host로 이동시키는 절차는 drain이다. application adapter(`FieldTransfer`, `DungeonTransfer`, `PlayerTransfer`)는 `preserveStateWith(...)`로 등록되어 게임 상태를 저장하고 복원하며 Framework는 workload와 route를 이전한다. `Hosting.run`은 `drain` 제어를 받으면 공개 planned-maintenance relocation을 요청한 뒤 종료 가능한 상태를 관찰하며, 절차는 [Relocation 가이드](https://docs.zlink.systems/ko/java/guide/server/37-relocation/)가 설명한다.

### 경계 알림과 timer

주기적으로 handler를 실행하는 timer를 사용하여 `Field.onInitialize`가 등록한 `BorderTick`이 경계 근처 플레이어 정보를 publish한다. 이 pub/sub 방식에서는 `BorderRoutes`의 handler가 구독한 정보를 받고, `Field.showBorder`가 관찰하는 Actor에게 보내며, 수신 Actor의 `Actions.deliver`가 binding된 Session으로 알림을 전달한다. `BorderRoutes`는 하나의 handler를 인접 필드 topic에 동적으로 등록한다. 주기 작업의 등록과 실행은 [Timers and workers](https://docs.zlink.systems/ko/java/guide/server/36-timer-worker/)가 설명한다.

## 요청 흐름

예시 게임의 요청은 다음 경로로 처리된다. 기능을 더하거나 옮기기 전에 이 경로를 따라 코드를 읽으면 바꿀 위치를 빠르게 찾을 수 있다.

1. client가 Session node에 STREAM으로 연결하고 첫 packet `JoinWorldRequest`를 보낸다. `GameSession`은 첫 packet 타입을 확인하고 `playerId`로 Player Actor를 `getOrCreate`한 뒤 `bindOrGet`으로 binding하고 원래 packet을 relay한다.
2. 새 Actor는 Entry Spot에서 시작한다. 공용 `EnterWorld.join`이 identity를 확인하고 `Player.move`를 호출하며, `Player.move`는 좌표에 해당하는 Field에 `joinSpot(Rules.fieldId(x), ...).defer()`를 요청한다.
3. `Field.onActorJoin`이 도착을 수락한다. 참가가 끝나면 `Player.onJoinCompleted`가 binding된 Session으로 `WorldState`를 보낸다. 이후 같은 client의 packet은 Actor가 속한 Field의 handler(`Actions.move` 등)가 처리한다. 이동이 다른 Field의 좌표에 닿으면 handler가 다시 `Player.move`를 호출하며, 다른 node에 있는 Field여도 client 연결은 바뀌지 않는다.
4. 경계 근처에서는 Field의 `BorderTick` timer가 `border` channel로 `BorderPresence`를 publish한다. 인접 Field의 `BorderRoutes` handler가 `Field.showBorder`를 호출하여 관찰하는 Actor에게 플레이어 ID로 `BorderVisible`을 보내고, `Actions.deliver`가 binding된 Session으로 전달한다.
5. `CreatePartyRequest`는 서버가 `party-` ID를 발급해 같은 ID의 Party User Spot을 만들고 `RequestToSpot`으로 `PartyCommand`를 보낸다. `JoinPartyRequest`는 Party Spot 존재를 확인한 뒤 Actor 참가를 요청한다. 파티 구성원은 Party Spot이 소유하며 Field를 나가면 해당 Field의 가시성만 제거한다.
6. Field의 `EnterDungeonRequest`는 `Field.enter`를 거쳐 Party Spot에서 파티장을 확인한다. 서버가 `run-` ID를 발급하고 `dungeon-<runId>`를 만든 뒤 `PrepareDungeon(runId, partySpotId, members)`를 보낸다. Preparing 또는 Entering 중 재요청은 현재 Run ID와 준비 상태를 재사용하고 아직 도착하지 않은 구성원에게만 `JoinDungeon`을 보낸다.
7. 각 구성원의 `travel` handler가 `joinSpot(dungeonId, ...).defer()`를 요청한다. `DungeonRun`이 Run ID와 구성원을 검사하고 Dungeon은 `DungeonMemberEntered`를 Party Spot으로 보낸다. 모두 도착하면 `RunStarted`, 마지막 stage에서는 `RunCompleted`를 보낸다. Dungeon 안의 `EnterDungeonRequest`는 현재 `DungeonView`를 반환하며 마을 전용 Party 요청은 `run-in-progress`로 거부한다.
8. node 교체는 `drain`으로 relocation을 요청한다. `FieldTransfer`는 가시성과 `FieldPositionDomain`의 남은 시간 스냅샷을 저장하고 `PartyTransfer`·`DungeonTransfer`·`PlayerTransfer`는 각 소유 상태를 보존한다. Framework가 다른 node에 workload를 복원한다.

## 템플릿 도메인 확장

이 템플릿의 도메인을 확장하여 MORPG 서버를 만든다. 기능을 추가하기 전에 README의 빌드와 e2e를 실행한다.

1. 이동 판정은 `FieldPositionDomain`, 가시성은 `FieldState`, 파티는 `PartyRun`, 던전은 `DungeonRun`에서 확장한다. 상태를 변경하는 소유자를 하나로 유지한다.
2. 새 공통 계약은 `server/shared/src/main/java/MyGame/`에 추가한다. 서버가 입력을 검사하고 결과를 계산한다. 거부된 입력은 값·순번·쿨다운을 변경하지 않는다.
3. 클라이언트 패킷만 `server/session/src/main/java/MyGame/session/GameSession.java`의 Session 허용 목록에 추가한다. 서버 간 메시지와 결과 메시지는 중계하지 않는다.
4. 관찰 시나리오는 `Scenarios.java`, 도메인 검사는 `DomainChecks.java`에 추가한다. README 시나리오 표와 `zlink.structure.json`의 `run.scenarios`를 함께 갱신한다. 시간 제한 거부는 서버가 반환한 `retryAfterMs`만 기다린다.
5. 상태 필드를 추가하면 해당 relocation 스냅샷과 경계 시험도 갱신한다. 시각은 프로세스 절대 시각 대신 남은 ms로 저장한다.

## 개발 시 주의점

### 상태와 메시지 계약

의존성은 `build.gradle.kts`에서 중앙으로 고정한 Maven Central 공개 package를 사용하며, project 의존은 이 프로젝트의 모듈 사이에 두며, 로컬 package feed와 저장소 소스에 대한 의존은 commit하지 않는다. typed 메시지는 기본 Framework JSON serializer로 처리하여 process마다 같은 계약을 사용한다. relocation adapter가 직접 직렬화하는 대상은 application snapshot으로 한정하여 Framework의 실행 상태와 게임 상태의 관리 책임을 분리한다.

`FieldPositionDomain`이 authoritative 좌표·sequence·속도 예산·도착 상태를 소유하고 `Field`는 가시성만 소유한다. `Player`는 현재 좌표와 Party Spot ID를 유지한다. `PartySpot`은 파티 구성과 run 상태를, `DungeonRun`은 준비된 run과 stage를 소유하며 Session binding은 connection identity를 소유한다. `Field`는 플레이어 ID로 가시성을 저장하고 Actor 참조는 보관하지 않는다. route, membership, lease, queue, 연결 복구와 execution gate는 Framework가 소유한다. 응답은 handler Actor에 binding된 Session으로 보내고 다른 push는 `ZLinkActorClient.sendToActor`로 플레이어 ID에 보낸다. `SessionInfo`는 Session binding에서 identity를 읽으며 게임 상태 메시지에는 binding identity를 넣지 않는다.

### 비동기 실행과 lifecycle

공개 비동기 호출은 템플릿처럼 `CompletionStage`로 연결하며, handler와 callback에서 `.join()`, `.get()`, `Thread.sleep`, blocking I/O로 실행을 중단하지 않는다. `Hosting.java`의 `.join()`은 시작·제어 thread에서 실행된다. Framework execution gate가 같은 상태를 직렬화하므로 별도 lock이나 queue로 다시 보호하지 않는다. `Program.java`는 `Field`, `Party`, `Dungeon`에 `SPOT_WIDE` 실행 모드를 지정하여 각 Spot과 멤버 Actor의 handler, timer, callback이 공통 gate를 사용하게 한다. await 중에도 turn을 유지할 수 있으므로 같은 Spot에 대한 request처럼 그 turn이 끝나야 실행되는 작업을 기다리지 않으며, 긴 작업은 handler 밖에서 처리한다. 실행 경계는 [Execution model](https://docs.zlink.systems/ko/java/guide/server/32-execution-model/)이 설명한다.

타입 등록과 객체별 callback은 정해진 lifecycle 단계에 둔다. `Field`는 시작 때 `fieldIds`로 `getOrCreate`하는 고정 User Spot이며 Party와 Dungeon은 동적 User Spot이다. Dungeon도 process 이름이 Instance일 뿐 User Spot이며, 생성과 초기화는 [Activation and lifetime](https://docs.zlink.systems/ko/java/guide/server/34-activation-lifetime/)이 설명한다. Message Follow와 safe-shutdown 관찰 기한은 `Control.java`의 공용 상수에 둔다. Spot 변경은 `joinSpot(...).defer()`와 `onJoinCompleted`를 사용하고 host 종료는 relocation과 runtime 관찰로 확인한다. handler는 각 `Program.java`의 `options.addHandlersFromPackageOf(...)`로 등록하며, 지정 package 밖의 새 handler class는 명시적으로 등록한다 (`field`는 공용 `MyGame.admission` handler도 등록하고 border route는 `Field.configure()`가 등록한다).

### 재접속과 실패 처리

reconnect에서는 같은 Actor ID를 사용하고 새 Session을 binding하여 기존 플레이어 상태를 이어서 사용한다. 현재 위치는 Framework가 관리하므로 application route cache를 추가하거나 reconnect 때 플레이어 상태를 초기화하지 않는다. 처리 용량 때문에 호출을 수락하지 못하거나 대기시키는 동작이 backpressure이며, [Backpressure 가이드](https://docs.zlink.systems/ko/java/guide/server/33-backpressure/)의 공개 결과를 관찰한다.

Framework 실패는 실제 reply·event·lifecycle 결과를 근거로 조사한다. Internal API·reflection·raw frame·사설 codec·추가 poller·고정 sleep·transport 재시도·deadline 확대·assertion 완화는 실패한 공개 동작을 검증하지 못하므로 해결 방법으로 추가하지 않는다. 코드 주석은 영어로 작성하고 실행·빌드 산출물인 `.logs/`, `build/`, `.gradle/`, `zlink.license` 파일은 commit에서 제외한다.

## 디버깅

### 실행별 로그

첫 실패 실행을 보존하고 로그를 읽은 뒤 재실행 여부를 결정한다. 간헐 실패도 실제로 관찰한 결함이며 이후 통과로 첫 실패를 제외하지 않는다. `.logs/run-<id>/`에는 `Nodes.java`가 node 시작 시 작성한 실제 설정과 node별 로그가 있다.

설정 파일은 `fieldA.json`, `fieldB.json`, `instanceA.json`, `instanceB.json`, `session.json`다. node를 시작할 때마다 `<node>-<uuid>/` 디렉터리가 만들어지며, 진단 stderr는 `<node>-stderr.log`에, 준비·제어 stdout은 `<node>-stdout.log`에 기록한다. 교체한 node(`update` 시나리오)는 새 디렉터리에 기록한다. 로컬 모드에서는 `compose.log`가 실행의 Redis Compose 출력을, `.logs/build.log`가 로컬 빌드 출력을 담는다. Docker 모드의 `.logs/docker-<project>/`에는 `build.log`, `redis-start.log`, `redis.log`, `cleanup.log`가 있다.

### 진단 수준

`server/shared/src/main/java/MyGame/Hosting.java`의 `configure`는 메시지 수신부터 처리 완료까지의 단계를 기록하는 message flow를 다음 설정으로 활성화한다.

```java
options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
```

`NORMAL`은 receive·dispatch·completion 단계와 오류를 기록한다. `ERRORS`(Framework 기본값)는 실패와 backpressure만 기록하고 `OFF`는 아무것도 기록하지 않는다. byte 크기와 경과 시간이 필요하면 `DETAILED`를 쓰며, 같은 dispatch option의 `includeMessageSizes(true)`로 메시지 크기도 기록할 수 있다. 상세 기록에는 비용이 추가되므로 필요한 조사 범위에서 사용한다.

`NodeConfig.java`는 diagnostics key를 정의하지 않으므로 시작 설정은 hosting 호출을 수정하고 다시 빌드·시작하여 적용한다. 실행 중에는 `ZLinkFrameworkRuntime`의 `messageFlowMode()`와 `setMessageFlowModeAsync`를 사용할 수 있지만 이 프로젝트의 stdin 제어 명령에는 연결되어 있지 않다. Framework handler와 callback에서는 비동기 메서드를 사용한다. 진단 항목과 설정은 [Monitoring](https://docs.zlink.systems/ko/java/guide/server/26-monitoring/)과 [Observability diagnostics](https://docs.zlink.systems/ko/java/reference/08-observability-diagnostics/)가 설명한다.

### Message flow 조사

`server/shared/src/main/resources/logback.xml`의 logback 설정이 console log를 stderr로 기록하므로 message flow는 node의 `<node>-stderr.log`에서 확인하며 별도 flow 파일을 만들지 않는다. message flow가 켜져 있으면 `Hosting.java`가 `RUNTIME state=...` 줄도 stderr에 기록한다. record의 `flow`는 node를 통과하는 메시지를 추적하는 값이고 `corr`는 request와 reply의 관계를 확인하는 값이다. record는 `zlink flow:`로 시작하며 실패는 `outcome=failed`, `error_type`, `error_message`로 표시된다.

실패 필드와 바로 앞 단계를 함께 읽고 category를 제외하지 않는다. 실행마다 flow ID가 달라지므로 통과·실패 실행은 논리적 작업, Actor ID와 단계 순서로 비교하여 처음 누락되거나 실패한 단계를 찾는다. startup이 message flow 기록 전에 실패했으면 시작 예외와 stack을 보존한다.

### 보조 trace와 정리

e2e client가 쓰는 STREAM connector를 추적하려면 `--local` 실행 전에 부모 process 환경에 `ZLINK_JAVA_STREAM_TRACE=1`을 설정한다. Docker runner와 node는 호스트 환경 변수를 상속하지 않으므로 Docker에서 쓰려면 Compose의 `runner` service에 변수를 추가한다. trace 줄은 `[zlink-java-stream-trace]` prefix를 가지며 `java.util.logging` 수준 `FINE`으로 기록되므로, 보려면 runner에서 그 수준을 활성화한다. PowerShell에서는 다음 환경 변수를 사용한다.

```powershell
$env:ZLINK_JAVA_STREAM_TRACE = "1"
```

Bash에서는 같은 변수를 다음과 같이 설정한다.

```bash
export ZLINK_JAVA_STREAM_TRACE=1
```

`ZLINK_JAVA_STREAM_TRACE`는 connector의 버전별 진단 switch이므로 routing API로 사용하지 않는다. runner는 자신이 만든 process와 Compose project를 정리하며, 정리 오류도 보존하고 다른 작업의 process나 container는 중단하지 않는다.

## ZLink 결함 보고

### 결함 판단

ZLink 결함은 공개 API를 문서대로 호출했는데 Framework가 문서와 다른 결과를 내는 경우다. 먼저 사용한 API를 위의 공개 가이드·샘플과 대조하여 application 사용 오류인지 확인한다. 사용 오류가 아니면 우회 코드를 추가하지 않고 issue로 보고하며, scenario assertion은 그대로 유지한다. 우회 코드는 결함을 감추고, 수정된 package로 올린 뒤에도 남아 동작을 바꾸기 때문이다.

### 기존 issue 확인

등록 전에 [ZLink issues](https://github.com/zlink-systems/zlink/issues?q=is%3Aissue)에서 오류 메시지·예외 타입·API 이름으로 열린 issue와 닫힌 issue를 함께 검색한다. 같은 결함이 있으면 새 issue를 만들지 않고 그 issue에 재현 환경과 로그를 덧붙인다. 닫힌 issue와 같은 결함이면 수정이 포함된 package 버전을 issue에서 확인하고, 이 프로젝트의 고정 버전을 올려 다시 실행한다.

### issue 본문

본문은 ZLink 저장소의 issue template 항목을 따른다. 다음 형식을 복사해 채운다.

```markdown
# Issue description
<scenario and the public operation that fails>

# Environment
* zlink version: <Framework and binding versions from build.gradle.kts>
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

제목에는 언어와 실패한 동작을 적는다(예: `framework(java): Spot startup fails with empty SpotId`). 등록한 issue 번호는 README의 Known issues에 적어, 수정 package가 나오면 다시 검증할 위치를 남긴다.
