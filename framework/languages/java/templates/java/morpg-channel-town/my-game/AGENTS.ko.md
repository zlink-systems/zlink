# MyGame 개발 가이드

## 프로젝트 개요

이 Java 25 프로젝트는 플레이어가 마을 채널에 모여 파티를 만들고 파티마다 전용 던전에 들어가는 MORPG 예제다. 연결을 처리하는 Session gateway, 고정 마을 채널을 가진 Town host A·B, Dungeon을 요청에 따라 만드는 Instance host A·B와 위치·이동 상태를 저장하는 Redis를 실행한다. `build.gradle.kts`는 ZLink Framework를 `0.30.2`, binding을 `1.20.0`으로 고정한다.

게임 기능을 추가할 위치는 아래의 프로젝트 구성에, 이 샘플의 기능을 자기 프로젝트로 옮기는 절차는 "이 샘플을 보며 개발하기"에 있다. 실행 환경·배포·공개 package의 알려진 문제는 [README](README.md)가 설명한다. 초기 node 구성과 scenario 목록은 [zlink.structure.json](zlink.structure.json)에 있으며, 실행 중 객체의 위치는 Framework와 Redis가 관리한다.

> **문서와 package 버전.** [ZLink 문서](https://docs.zlink.systems/ko/), [Java 가이드](https://docs.zlink.systems/ko/java/guide/server/), [API reference](https://docs.zlink.systems/ko/java/reference/)와 [에이전트 색인](https://docs.zlink.systems/llms.txt)이 공개 문서의 진입점이다. 현재 가이드가 더 새 버전을 설명하면 `build.gradle.kts`에 고정된 버전의 API와 대조한다.

## 개발 명령

기본 실행은 Docker에서 node와 client를 모두 실행하므로 Docker(Compose v2)가 필요하고 호스트 JDK는 필요 없다. 이 디렉터리에서 다음 명령을 실행한다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario channel,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash도 같은 절차를 실행한다.

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario channel,dungeon,reconnect,update
./run-e2e.sh --check-structure
```

로컬 process 디버깅은 JDK 25를 설치한 뒤 `--local` 또는 `-Local`로 실행한다. 로컬 빌드가 있으면 `--no-build` 또는 `-NoBuild`를 추가한다. 시나리오 순서와 assertion은 유지하며 최초 실패를 README의 Known issues와 대조한다.

## 프로젝트 구성과 확장 위치

### Session gateway

`server/session/src/main/java/MyGame/session/Program.java`는 STREAM과 mesh listener를 등록하고 `server/session/src/main/java/MyGame/session/GameSession.java`는 연결 callback과 수신 packet 전달을 담당한다. 연결별 통신 객체가 Session이며, Framework는 Session을 플레이어 상태 객체와 연결하는 binding을 제공한다. `GameSession.onDispatch`는 클라이언트 허용 목록 밖의 packet을 거부하고 첫 packet으로 `JoinTownRequest`를 요구하며, `playerId`로 Player Actor를 구해 binding한 뒤 원래 packet을 relay한다. 인증은 binding 전에 추가하고 typed relay는 유지하여 플레이어에게 응답할 연결을 Framework가 확인하게 한다.

### Town과 플레이어

ID로 찾는 게임 상태 객체가 Actor이며, `server/shared/src/main/java/MyGame/Player.java`의 `Player`는 공용 플레이어 Actor로 application 상태가 없다. Actor를 받아 공통 게임 규칙을 실행하는 상태 객체가 Spot이며, `server/town/src/main/java/MyGame/town/Town.java`의 `Town`은 고정 마을 채널 하나(`town-<channel>`)로 그 채널의 플레이어와 파티(`server/town/src/main/java/MyGame/town/TownState.java`)를 소유한다. 공용 application Entry Spot(`server/shared/src/main/java/MyGame/admission/Entry.java`, `server/shared/src/main/java/MyGame/admission/EntryActions.java`)이 플레이어의 첫 진입을 요청한 채널로 보낸다.

`server/town/src/main/java/MyGame/town/Program.java`가 Entry Spot, Town factory(`townCapacity`), Player factory와 공용 entry handler를 등록한다. `server/town/src/main/java/MyGame/town/TownActions.java`에는 login·채널 변경·파티·던전 입장·전달 handler가 있다. node 구성을 바꾸면 실제 등록과 `server/config/`, descriptor를 함께 맞추어 구조 검사가 같은 구성을 검사하게 한다.

### Dungeon

`server/instance/src/main/java/MyGame/instance/Program.java`가 Dungeon factory(`server/config/instanceA.json`의 `dungeonCapacity`)와 Entry Spot을 등록한다. `server/instance/src/main/java/MyGame/instance/Dungeon.java`의 `Dungeon`은 application이 생성하는 User Spot이며 Framework가 별도 생성·초기화 계약으로 제공하는 Instance Spot과 구분되고, 파티 구성원·참가자·run 단계·게임 단계(`DungeonState`)를 소유한다. `server/instance/src/main/java/MyGame/instance/Prepare.java`는 멱등 파티 run 준비 요청에 응답하고 `server/instance/src/main/java/MyGame/instance/DungeonActions.java`에는 재접속·현재 상태 조회·단계 진행 handler가 있다. 전투·보상·Dungeon lifecycle은 이 프로젝트에 추가한다.

### 공통 계약과 hosting

`server/shared/src/main/java/MyGame/`는 process 간 메시지와 게임 상수(`JoinTownRequest`, `ChangeChannelRequest`, `CreatePartyRequest`, `JoinPartyRequest`, `EnterDungeonRequest`, `AdvanceRequest`, `ViewRequest`, `TownView`, `PartyView`, `PrepareDungeon`, `DungeonPrepared`, `JoinDungeon`, `RunStarted`, `RunCompleted`, `PartyViewRequest`, `DungeonView`, `ActionRejected`, `RunPhase`)를, `PartyRun.java`는 파티 실행 상태 기계와 서버 발급 ID를, `ChannelRules.java`는 채널 입력 검사를 소유한다. `NodeConfig.java`는 process 설정을, `Names.java`는 mesh와 타입 이름을 정의한다. `Hosting.java`는 hosting과 진단을 설정하고 stdin 제어 loop(`drain`, `shutdown`)를 실행하며, `Control.java`는 제어 marker와 기한을, `Stores.java`는 Redis Location·Relocation store를 만든다. `NodeRole.java`는 node 역할을, `Rules.java`는 파티와 거부 규칙을, `Player.java`·`PlayerFactory.java`·`PlayerTransfer.java`는 공용 Player Actor를, `Transfer.java`는 relocation adapter의 취소 검사를 담는다. 여러 process가 사용하는 계약은 여기에 추가하며, 파티와 구성원 상태는 `Town`과 `Dungeon`에 유지하여 서로 다른 process가 같은 값을 따로 변경하지 않게 한다.

### E2E client

`e2e/src/main/java/MyGame/e2e/Client.java`는 공개 connector로 게임에 접속하고 `e2e/src/main/java/MyGame/e2e/Scenarios.java`는 client가 관찰할 결과를 검사한다. `e2e/src/main/java/MyGame/e2e/Nodes.java`는 node를 시작하고 로그를 보존하며 실행별 자원을 정리한다. `e2e/src/main/java/MyGame/e2e/Program.java`는 시나리오 실패 다음에 정리 실패를 보고하며 둘 중 하나라도 있으면 종료 코드는 1이다. `e2e/src/main/java/MyGame/e2e/Structure.java`는 시작 전에 실제 파일을 검사한다. 시나리오 순서 `channel,dungeon,reconnect,update`를 유지한다. 새 게임 동작은 client가 받은 결과로 검증하며, unit test `server/town/src/test/java/MyGame/town/TownStateTest.java`, `server/instance/src/test/java/MyGame/instance/DungeonStateTest.java`, `server/instance/src/test/java/MyGame/instance/DomainChecksTest.java`가 Framework 없이 도메인 규칙을 검사한다. `./gradlew --no-daemon :server:town:test :server:instance:test`로 실행한다. formatter 설정은 포함되어 있지 않다.

## 프로젝트의 ZLink 개념

### RouteMesh

process 간 메시지를 전달할 node들을 연결하는 구성이 RouteMesh이며, 이 프로젝트에서는 `Names.MESH`(`my-game-world`)를 사용한다. 등록과 호출은 [Channel messaging](https://docs.zlink.systems/ko/java/guide/server/20-channel-messaging/)에서 설명한다.

### Spot과 Actor

`Entry`는 플레이어의 첫 진입을 받고 `Town`은 고정 마을 채널 하나를, `Dungeon`은 파티 하나를 위해 만든 던전을 표현한다. [Spot 가이드](https://docs.zlink.systems/ko/java/guide/server/21-spot/)는 이 객체들의 등록과 호출을 설명한다. `Player`는 소속 Spot이 바뀌어도 연결 접근을 유지하며, 생성과 호출은 [Actor 가이드](https://docs.zlink.systems/ko/java/guide/server/22-actor/)에서 설명한다.

### STREAM과 Session

client 연결의 packet을 처리하는 [STREAM](https://docs.zlink.systems/ko/java/guide/server/23-stream/)에서 `GameSession`은 Player Actor를 구해 binding한 뒤 원래 typed packet을 전달한다. Actor handler는 binding된 Session을 통해 client에 메시지를 보낸다. 연결과 Actor 사이의 관계는 [Session and Actor](https://docs.zlink.systems/ko/java/guide/server/24-actor-session/)가 설명한다.

### Location Store

객체 ID에 해당하는 현재 실행 위치를 저장하는 저장소가 Location Store이며, 이 프로젝트는 `server/shared/src/main/java/MyGame/Stores.java`가 만드는 Redis 구현을 사용하고 `docker-compose.yml`이 e2e 실행의 Redis service를 정의한다. application은 node 주소 대신 Actor와 Spot의 ID로 호출하므로 위치가 바뀌어도 호출 대상을 같은 ID로 지정한다. 조회 방식은 [Location 가이드](https://docs.zlink.systems/ko/java/guide/server/25-location/)가 설명한다.

### Actor membership

Actor가 소속 Spot을 바꾸는 동작은 membership 변경이다. 대상 Spot이 참가 요청의 수락 여부를 정하는 단계가 admission이다. 공용 `EntryActions.join`, `TownActions.change`, `TownActions.travel`이 Town 또는 Dungeon 참가를 지연 처리로 요청하고 `Town.onActorJoin`과 `Dungeon.onActorJoin`이 수락 여부를 정하며 `Player.onJoinCompleted`는 완료 결과를 확인한다. 참가 수락과 완료의 순서는 [Actor membership](https://docs.zlink.systems/ko/java/guide/server/35-actor-membership/)이 설명한다.

### Host relocation과 drain

객체의 실행 host를 변경하는 동작은 relocation이며, host를 종료할 수 있도록 workload를 다른 host로 이동시키는 절차는 drain이다. application adapter(`TownTransfer`, `DungeonTransfer`, `PlayerTransfer`)는 `preserveStateWith(...)`로 등록되어 게임 상태를 저장하고 복원하며 Framework는 workload와 route를 이전한다. `Hosting.run`은 `drain` 제어를 받으면 공개 planned-maintenance relocation을 요청한 뒤 종료 가능한 상태를 관찰하며, 절차는 [Relocation 가이드](https://docs.zlink.systems/ko/java/guide/server/37-relocation/)가 설명한다.

## 요청 흐름

예시 게임의 요청은 다음 경로로 처리된다. 기능을 더하거나 옮기기 전에 이 경로를 따라 코드를 읽으면 바꿀 위치를 빠르게 찾을 수 있다.

1. client가 Session node에 STREAM으로 연결하고 첫 packet `JoinTownRequest`(`playerId`, 64-bit `channel`)를 보낸다. `GameSession`은 허용 목록 밖 packet을 거부하고 `playerId`로 Player Actor를 `getOrCreate`한 뒤 `bindOrGet`으로 binding하고 원래 packet을 relay한다.
2. 새 Actor는 Entry Spot에서 시작한다. 공용 `EntryActions.join`가 identity와 채널 범위를 확인하고 `joinSpot(Names.townId(channel), request).defer()`를 요청한다. `Town.onActorJoin`이 플레이어와 채널을 확인하여 플레이어를 추가하고 `Town.onJoinedActor`가 채널에 `TownView`를 broadcast한다.
3. `ChangeChannelRequest`는 `TownActions.change`에 전달되어 잘못된 채널과 실행 중인 파티 구성원을 거부한 뒤 다른 마을 채널에 `joinSpot(...).defer()`를 요청한다. Forming 상태의 파티 구성원이 마을을 떠나면 파티에서도 제거된다. `CreatePartyRequest`는 서버 발급 PartyId로 `PartyRun`을 만들고 `JoinPartyRequest`는 Forming 파티에만 구성원을 추가한다.
4. `EnterDungeonRequest`는 `Town.enter`에 전달되며 파티장의 요청만 수락한다. Preparing·Entering 재요청은 같은 RunId를 쓰고 `getOrCreate`로 `dungeon-<runId>`를 준비한다. `PrepareDungeon`에는 owner Town ID와 구성원 명단이 들어가며, 준비 응답이 일치하면 아직 Town에 있는 구성원에게 `JoinDungeon`을 보낸다.
5. 각 구성원의 `travel` handler가 `joinSpot(dungeonId, ...).defer()`를 요청한다. `Dungeon.onActorJoin`은 RunId와 명단이 맞는 구성원만 수락한다. 모든 구성원이 도착하면 Dungeon이 Active가 되어 owner Town에 `RunStarted`를 보내고, 3단계에서 Completed가 되어 `RunCompleted`를 보낸다. Dungeon 상태 조회는 owner Town에서 현재 `PartyView`도 가져온다.
6. node를 교체할 때 `Hosting.run`의 `drain` 제어가 relocation을 요청한다. Framework는 relocation adapter가 저장한 PartyRun과 Dungeon의 owner·run·phase 상태를 다른 node에서 복원한다.

## 템플릿 도메인 확장

이 템플릿의 도메인을 확장하여 MORPG 서버를 만든다. 실행과 배포 절차는 [README](README.md)를 따른다.

1. `server/town/src/main/java/MyGame/town/TownState.java`에서 채널 인원 규칙을, `server/shared/src/main/java/MyGame/PartyRun.java`에서 파티 run 전이를, `server/instance/src/main/java/MyGame/instance/DungeonState.java`에서 던전 입장·phase·단계 규칙을 확장한다. 각 상태의 판정 위치를 도메인 한 곳에 유지한다.
2. 새 공통 계약은 `server/shared/src/main/java/MyGame/`에 추가한다. 클라이언트 입력과 서버 내부 메시지를 구분하고 기본 typed serializer를 사용한다.
3. 새 클라이언트 패킷은 `server/session/src/main/java/MyGame/session/GameSession.java`의 Session 허용 목록에 추가한다. 내부 메시지는 클라이언트가 보낼 수 없다.
4. `e2e/src/main/java/MyGame/e2e/Scenarios.java`에 응답과 거부 뒤 상태 불변을 관찰하는 시나리오를 추가한다. README의 시나리오 표와 `zlink.structure.json`의 `run.scenarios`를 함께 맞춘다.

상태 필드를 추가할 때 relocation capture와 restore를 함께 수정한다. Framework가 소유한 라우팅·membership·binding 결정을 도메인에 재구현하지 않는다.

## 개발 시 주의점

### 상태와 메시지 계약

의존성은 `build.gradle.kts`에서 중앙으로 고정한 Maven Central 공개 package를 사용하며, project 의존은 이 프로젝트의 모듈 사이에 두며, 로컬 package feed와 저장소 소스에 대한 의존은 commit하지 않는다. typed 메시지는 기본 Framework JSON serializer로 처리하여 process마다 같은 계약을 사용한다. relocation adapter가 직접 직렬화하는 대상은 application snapshot으로 한정하여 Framework의 실행 상태와 게임 상태의 관리 책임을 분리한다.

`Town`이 채널의 플레이어와 파티를, `Dungeon`이 파티 구성원과 단계를, Session binding이 identity를 소유한다. Spot 상태에는 플레이어 ID를 보관하고 Actor 참조는 보관하지 않는다. host relocation은 대상에서 `onJoinedActor`를 다시 호출하지 않기 때문이다. route, membership, lease, queue, scheduling, 배치와 relocation은 Framework가 소유한다. 요청자에게는 handler가 받은 Actor의 binding된 Session(`actor.context().boundSession().send(...)`)으로 응답한다. 그 밖의 push는 `ZLinkActorClient.sendToActor`로 플레이어 ID의 Actor에게 보내고, `TownActions.deliver` 같은 Spot Actor handler가 binding된 Session으로 전달한다. `SessionInfo`는 Session binding에서 identity를 읽으며, 게임 상태 메시지에는 binding identity를 넣지 않는다.

### 비동기 실행과 lifecycle

공개 비동기 호출은 템플릿처럼 `CompletionStage`로 연결하며, handler와 callback에서 `.join()`, `.get()`, `Thread.sleep`, blocking I/O로 실행을 중단하지 않는다. `Hosting.java`의 `.join()` 호출은 시작·제어 thread에서 실행되며 handler 안이 아니다. Framework의 execution gate가 같은 상태에 대한 turn의 동시 실행을 제한하므로 별도 lock이나 queue로 같은 상태를 다시 보호하지 않는다. `Program.java`는 `Town` and `Dungeon`에 `SPOT_WIDE` 실행 모드를 지정하여 해당 Spot과 멤버 Actor의 handler, timer, callback이 공통 gate를 사용하게 한다. await 중에도 turn을 유지할 수 있으므로 같은 Spot에 대한 request처럼 그 turn이 끝나야 실행되는 작업을 기다리지 않으며, 긴 작업은 handler 밖에서 처리한다. 실행 경계는 [Execution model](https://docs.zlink.systems/ko/java/guide/server/32-execution-model/)이 설명한다.

타입 등록과 객체별 callback은 각각 정해진 lifecycle 단계에 둔다. `Town` 채널은 `Hosting.run`이 시작할 때 `townIds`로 `getOrCreate`하는 고정 User Spot이고, `Dungeon`은 process 이름이 Instance여도 User Spot이며, 생성과 초기화는 [Activation and lifetime](https://docs.zlink.systems/ko/java/guide/server/34-activation-lifetime/)이 설명한다. Message Follow 기간과 거기서 유도한 safe-shutdown 관찰 기한은 `Control.java`의 공용 상수에 둔다. Spot 변경은 `joinSpot(...).defer()`와 공개 완료 callback `onJoinCompleted`를 사용하고 host 종료는 relocation과 runtime 관찰로 확인하여 요청 수락을 완료로 오인하지 않는다. handler는 각 `Program.java`의 `options.addHandlersFromPackageOf(...)`로 등록하며, 이 호출이 포함하는 package 밖의 새 handler class는 직접 등록하지 않으면 등록되지 않는다 (`town`은 공용 `MyGame.admission` handler도 함께 등록한다).

### 재접속과 실패 처리

reconnect에서는 같은 Actor ID를 사용하고 새 Session을 binding하여 기존 플레이어 상태를 이어서 사용한다. 현재 위치는 Framework가 관리하므로 application route cache를 추가하거나 reconnect 때 플레이어 상태를 초기화하지 않는다. 처리 용량 때문에 호출을 수락하지 못하거나 대기시키는 동작이 backpressure이며, [Backpressure 가이드](https://docs.zlink.systems/ko/java/guide/server/33-backpressure/)의 공개 결과를 관찰한다.

Framework 실패는 실제 reply·event·lifecycle 결과를 근거로 조사한다. Internal API·reflection·raw frame·사설 codec·추가 poller·고정 sleep·transport 재시도·deadline 확대·assertion 완화는 실패한 공개 동작을 검증하지 못하므로 해결 방법으로 추가하지 않는다. 코드 주석은 영어로 작성하고 실행·빌드 산출물인 `.logs/`, `build/`, `.gradle/`, `zlink.license` 파일은 commit에서 제외한다.

`Town.onDisconnectActor`는 명시적 이동까지 마을과 파티 membership을 유지하므로, 같은 Actor ID로 재접속하면 같은 채널로 돌아온다.

## 디버깅

### 실행별 로그

첫 실패 실행을 보존하고 로그를 읽은 뒤 재실행 여부를 결정한다. 간헐 실패도 실제로 관찰한 결함이며 이후 통과로 첫 실패를 제외하지 않는다. `.logs/run-<id>/`에는 `Nodes.java`가 node 시작 시 작성한 실제 설정과 node별 로그가 있다.

설정 파일은 `instanceA.json`, `instanceB.json`, `session.json`, `townA.json`, `townB.json`다. node를 시작할 때마다 `<node>-<uuid>/` 디렉터리가 만들어지며, 진단 stderr는 `<node>-stderr.log`에, 준비·제어 stdout은 `<node>-stdout.log`에 기록한다. 교체한 node(`update` 시나리오)는 새 디렉터리에 기록한다. 로컬 모드에서는 `compose.log`가 실행의 Redis Compose 출력을, `.logs/build.log`가 로컬 빌드 출력을 담는다. Docker 모드의 `.logs/docker-<project>/`에는 `build.log`, `redis-start.log`, `redis.log`, `cleanup.log`가 있다.

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
