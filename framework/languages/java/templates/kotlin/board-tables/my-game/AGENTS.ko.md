# MyGame 개발 가이드

## 프로젝트 개요

이 Kotlin 프로젝트(Java 25 toolchain 위의 Kotlin 2.3.21, Gradle 빌드)는 4인 보드 테이블 server다. 연결을 처리하는 Session gateway, Table host 둘(`tableA`·`tableB`)과 위치·이전 상태를 저장하는 Redis를 실행한다. `build.gradle.kts`는 ZLink Framework package를 `0.30.2`(`frameworkVersion`)로, ZLink binding을 `1.20.0`(`bindingVersion`)으로 고정한다.

게임 기능은 아래의 프로젝트 구성과 확장 절차에 따라 추가한다. 실행 환경·배포·scenario별 검사 내용은 [README](README.md)가 설명한다. 초기 node 구성과 scenario 목록은 [zlink.structure.json](zlink.structure.json)에 있으며, 실행 중 객체의 위치는 Framework와 Redis가 관리한다.

> **문서와 package 버전.** [ZLink 문서](https://docs.zlink.systems/ko/), [Kotlin 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/), [API reference](https://docs.zlink.systems/ko/kotlin/reference/)와 [에이전트 색인](https://docs.zlink.systems/llms.txt)이 공개 문서의 진입점이다. 현재 가이드가 더 새 버전을 설명하면 `build.gradle.kts`에 고정된 package의 API와 대조한다.

## 개발 명령

기본 실행은 Docker에서 node와 client를 모두 실행하므로 호스트 JDK가 필요 없다. 이 디렉터리에서 다음 명령을 실행한다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario table,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash도 같은 절차를 실행한다.

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario table,reconnect,update
./run-e2e.sh --check-structure
```

로컬 process 디버깅은 JDK 25를 설치한 뒤 `--local` 또는 `-Local`로 실행한다. 로컬 빌드가 있으면 `--no-build` 또는 `-NoBuild`를 추가하며, 이 옵션은 `--local`/`-Local`과 함께만 쓴다. 시나리오 순서와 assertion은 유지하며 최초 실패를 README와 대조한다. 빌드와 e2e는 Windows와 Linux 모두에서 실행한다. runner는 node를 시작하기 전에 구조를 검사하며, 실행마다 고유한 Redis key prefix, Compose project, OS가 배정한 포트를 쓰므로 runner를 고칠 때도 이 격리를 유지한다.

## 프로젝트 구성과 확장 위치

### Session gateway

`server/session/src/main/kotlin/MyGame/session/Program.kt`는 STREAM node와 RouteMesh client를 등록하고, `GameSession`(같은 파일)이 수신 packet을 처리한다. 연결별 통신 객체가 Session이며, Framework는 Session을 플레이어 상태 객체와 연결하는 binding을 제공한다. `GameSession`은 binding에서 `SessionInfoRequest`에 응답하고, `JoinTableRequest`를 받으면 Player Actor를 구하거나 만들어 binding하며, 그 밖의 packet은 binding된 Actor로 relay한다. 인증은 binding 전에 추가하고 typed packet relay는 유지하여 플레이어에게 응답할 연결을 Framework가 확인하게 한다.

### Table과 플레이어

ID로 찾는 게임 상태 객체가 Actor이며, `server/table/src/main/kotlin/MyGame/table/Player.kt`의 `Player`는 게임 상태를 갖지 않는 최소 Actor다. Actor를 받아 공통 게임 규칙을 실행하는 상태 객체가 Spot이며, `server/table/src/main/kotlin/MyGame/table/Table.kt`의 `Table`이 좌석(`Seat`), 칩, 카드, 턴, 팟을 `TableState`로 관리하는 Table User Spot이다. 숨겨야 할 정보는 Table에 둔다. `HandDealt`는 플레이어의 카드를 그 플레이어에게만 보내고 `TableView`는 카드 수만 담는다.

`server/table/src/main/kotlin/MyGame/table/Program.kt`가 handler, Entry Spot, `Table` factory, `Player` factory를 등록한다. `Table`은 기본 `SPOT_WIDE` 실행 모드를 쓰며, 선택 설정 `tableCapacity`는 `stableTypeLimit`로 node당 Table 수를 제한한다(생략하거나 0이면 제한이 없다). `Player.kt`의 `Entry`와 `Join`이 플레이어 진입을 처리하고, `Table.kt`의 `Actions` 클래스가 플레이어 요청을 Table 규칙에 연결하는 handler를 가진다. `Transfer.kt`는 relocation adapter용 `ensureActive`를 정의한다. node 구성을 바꾸면 실제 등록과 `server/config/`, descriptor를 함께 맞추어 구조 검사가 같은 구성을 검사하게 한다.

### 공통 계약과 hosting

`server/shared/src/main/kotlin/MyGame/Contracts.kt`는 process 간 이름·게임 상수·메시지를 정의한다. `Hosting.kt`는 `NodeConfig`(process 설정), Redis store, hosting과 진단을 설정하는 `configure`, node를 시작하고 stdin 제어 명령을 처리하는 `runNode`를 정의하고, `Control.kt`는 제어·준비 문자열을 정의한다. `Names`와 `Rules`는 node 이름, 거부 사유, 게임 상수를 가진다. hosting과 제어 protocol은 role마다 복제하지 않고 이곳에 둔다. 여러 process가 사용하는 계약은 `Contracts.kt`에 추가하며, 테이블 상태는 `Table`에 유지하여(Player Actor는 사본을 갖지 않는다) 서로 다른 process가 같은 값을 따로 변경하지 않게 한다.

### E2E client

`e2e/src/main/kotlin/MyGame/e2e/Client.kt`는 공개 stream connector로 게임에 접속하고 `Scenarios.kt`는 client가 관찰할 결과를 검사한다. `Nodes.kt`는 실행별 process와 Compose project를 관리하고, `Structure.kt`는 구성을 검사하며, `Program.kt`는 시나리오를 고르고 종료 코드를 정한다(시나리오나 정리가 실패하면 종료 코드는 1이다). `DockerRuntime.kt`는 Docker 모드에서 node를 container로 실행하고 `Cleanup.kt`는 정리 실패를 모은다. 새 게임 동작은 client가 받은 결과로 검증한다. 별도 unit-test 모듈은 포함되어 있지 않다.

## 프로젝트의 ZLink 개념

### RouteMesh와 channel

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.MESH` (`my-game-tables`). Channels distinguish messages by purpose and name; this project declares no application channels (`channels` is empty in `zlink.structure.json`). [Channel messaging](https://docs.zlink.systems/ko/kotlin/guide/server/20-channel-messaging/) describes registration and calls.

### Spot과 Actor

`Entry` handles Actor entry and `Table` is a dynamically created User Spot that owns a table's state. The [Spot 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/21-spot/) describes registration and calls for these objects. `Player` is addressed by player ID and keeps no copy of table state; the [Actor 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/22-actor/) describes Actor creation and calls.

### STREAM과 Session

With [STREAM](https://docs.zlink.systems/ko/kotlin/guide/server/23-stream/), which processes packets on client connections, `GameSession` obtains the Player Actor, binds it, and relays the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/ko/kotlin/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation (`ZLinkRedisLocationStore` in `Hosting.kt`). Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `Join.join` requests deferred admission to the table with `joinSpot(...).defer()`, `Table.onActorJoinSuspending` seats the player or rejects a full table, and `Player.onJoinCompletedSuspending` reports a rejection to the client. [Actor membership](https://docs.zlink.systems/ko/kotlin/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation과 drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `runNode` in `Hosting.kt` requests host relocation when it reads `drain` and reports `SAFE_TO_SHUTDOWN` after observing it; `TableTransfer` stores `TableState` as JSON and `PlayerTransfer` stores nothing. The [Relocation 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/37-relocation/) explains the procedure.

## 요청 흐름

예시 게임의 요청은 다음 경로로 처리된다. 기능을 더하거나 옮기기 전에 이 경로를 따라 코드를 읽으면 바꿀 위치를 빠르게 찾을 수 있다.

1. client가 Session node에 STREAM으로 연결하고 첫 패킷 `JoinTableRequest`를 보낸다. `GameSession`은 `playerId`로 Player Actor를 `getOrCreate`한 뒤 `bindOrGet`으로 Session에 binding하고 원래 패킷을 relay한다.
2. 새 Actor는 Entry Spot `Entry`에서 시작한다. `Join.join`이 identity를 확인하고 `tableId`로 `getOrCreate`하여 table을 만든 뒤 `joinSpot(...).defer()`를 요청한다.
3. `Table.onActorJoinSuspending`이 플레이어를 앉히거나 table이 가득 차면 참가를 거부한다. 참가가 수락되면 `Table.onJoinedActorSuspending`이 앉은 모든 플레이어에게 `TableView`를 보내고, 거부는 `Player.onJoinCompletedSuspending`이 `ActionRejected`로 client에 전달한다.
4. 이후 패킷(`TableViewRequest`, `StartHandRequest`, `BetRequest`)은 Actor가 속한 Table의 `Actions` handler가 처리한다. 응답은 요청자의 binding된 Session으로 보내고, 다른 앉은 플레이어에게 가는 push는 `sendToActor`를 쓰며 받는 쪽 `Actions` handler(`tableView`, `dealt`, `result`)가 그 Actor의 binding된 Session으로 전달한다.
5. 재접속한 client는 새 Session에서 `JoinTableRequest`를 다시 보낸다. 같은 Actor가 binding되고, `Table.admit`은 이미 앉은 플레이어를 새 좌석 없이 받으며, `Actions.join`이 현재 view와 플레이어의 카드를 다시 보낸다.
6. node를 교체할 때 `runNode`의 `drain`이 relocation을 요청한다. Framework는 `TableTransfer`·`PlayerTransfer`의 `capture`로 게임 상태를 받아 다른 node에서 `restore`한다.

## 도메인 확장

이 템플릿의 도메인을 확장하여 보드게임 서버를 만든다. 아래 파일이 현재 규칙의 판정과 상태를 소유하며, 새 규칙도 해당 위치에 추가한다.

### 템플릿 먼저 실행하기

규칙을 추가하기 전에 [개발 명령](#개발-명령)으로 빌드와 e2e를 실행한다. 실패하면 [디버깅](#디버깅)의 로그와 [README](README.md)의 Known issues에서 첫 실패 원인을 확인한다.

### 게임 규칙 추가

1. typed request와 result class는 `server/shared/src/main/kotlin/MyGame/Contracts.kt`에 추가한다. 공통 정수 상한과 거부 사유도 이 파일에 둔다. client는 행동을 요청하고, `Table`이 입력을 검사해 결과 상태를 계산한다.
2. 도메인 상태와 판정은 `server/table/src/main/kotlin/MyGame/table/Table.kt`에 추가한다. `TableState`와 `Seat`가 상태를 보유하고 `Table`이 판정한다. `TableTransfer`가 그 상태를 저장한다. 시간 규칙은 이 도메인 판정 한 곳에서 단조 시계로 계산한다. 시험용 시계는 `Table` 생성자로만 전달하고 relocation 상태에는 남은 ms를 저장한다.
3. client가 보낼 수 있는 request만 `server/session/src/main/kotlin/MyGame/session/Program.kt`의 허용 목록에 추가한다. 허용 목록 밖의 packet은 `packet-not-allowed`로 거부한다.
4. client가 관찰할 수 있는 검사는 `e2e/src/main/kotlin/MyGame/e2e/Scenarios.kt`에 추가한다. `table`·`reconnect`·`update` 동작을 유지한다. 시나리오 ID를 추가할 때는 `Contracts.kt`의 `Names.SCENARIOS`, `zlink.structure.json`, README 표도 함께 맞춘다.

서버가 행동을 검사하고 카드 분배, 차례, 칩 잔액과 정산 결과를 결정한다. 거부된 행동은 테이블 값, 순번과 cooldown 상태를 바꾸지 않는다. 거부 응답은 `ActionRejected(reason, retryAfterMs)` 형태이며 JSON 필드는 `reason`과 `retryAfterMs`다. 현재 거부의 대기값은 0이다. 양수 대기 시간이 있으면 client는 서버가 준 값만 기다린다. 고정 대기나 기한 확대로 시나리오를 통과시키지 않는다.
## 개발 시 주의점

### 상태와 메시지 계약

의존성은 `build.gradle.kts`에 고정한 Maven Central의 공개 package를 사용하며, 프로젝트 의존은 이 프로젝트의 Gradle 모듈 사이에 둔다. typed 메시지는 기본 Framework JSON serializer로 처리하여 process마다 같은 계약을 사용한다. relocation adapter(`*Transfer` 클래스)가 직접 직렬화하는 대상은 application 상태로 한정하여 Framework의 실행 상태와 게임 상태의 관리 책임을 분리한다. route·lease·membership·completion·queue·execution gate는 Framework가 관리하므로 application 상태에 같은 결정을 복제하지 않는다.

좌석의 칩 같은 게임 사실은 그것을 변경하는 객체 한 곳(`Table`)에서 관리한다. Table 상태를 Player Actor에 복사하지 않는다. Actor handler 안의 응답은 그 Actor의 binding된 Session으로 보내고, 그 밖의 push는 공개 Actor client로 Actor를 플레이어 ID로 지정하여 보내며 받는 Actor handler가 binding된 Session으로 전달한다. Spot은 나중의 전달을 위해 Actor 참조를 보관하지 않는다. host relocation이 이전 대상에서 `onJoinedActor`를 다시 호출하지 않기 때문이다. Actor identity는 Session binding이 소유하므로 게임 메시지에 중복해서 담지 않는다(`SessionInfo`는 현재 binding을 읽는다). Session은 binding 뒤 packet을 전달하고, 게임 판정은 해당 상태를 관리하는 Actor나 Spot에서 수행한다.

### 비동기 실행과 lifecycle

공개 호출은 템플릿처럼 `await()`로 기다리며, handler와 callback에서 `.join()`, `.get()`, `runBlocking`, `Thread.sleep` 같은 blocking 호출을 쓰지 않는다. `Table`은 기본 `SPOT_WIDE` 실행 모드를 사용하므로 같은 Table의 Spot·멤버 Actor handler와 lifecycle callback이 공통 execution gate를 사용한다. 같은 상태를 별도 lock이나 queue로 다시 보호하지 않는다. await 중에도 turn을 유지할 수 있으므로 같은 Spot에 대한 request처럼 그 turn이 끝나야 실행되는 작업을 기다리지 않으며, 긴 작업은 handler 밖에서 처리한다. 실행 경계는 [Execution model](https://docs.zlink.systems/ko/kotlin/guide/server/32-execution-model/)이 설명한다.

타입 등록과 객체별 callback은 각각 정해진 lifecycle 단계에 둔다. `Table`은 `Join.join`이 `getOrCreate`로 요청에 따라 만드는 User Spot이며, 생성과 초기화의 차이는 [Activation and lifetime](https://docs.zlink.systems/ko/kotlin/guide/server/34-activation-lifetime/)이 설명한다. membership 변경은 `joinSpot(...).defer()`와 공개 완료 callback(`onJoinCompletedSuspending`)을 사용하고 host 종료는 relocation과 runtime 관찰(`drain` 다음 `SAFE_TO_SHUTDOWN`)로 확인하여 요청 수락을 완료로 오인하지 않는다. `Control.kt`는 safe-shutdown 대기(`SAFE_DEADLINE`)를 Message Follow 시간(`MESSAGE_FOLLOW`)과 host deadline(`HOST_DEADLINE`)에서 유도하므로 이 값들은 그곳에서만 바꾼다.

### 재접속과 실패 처리

reconnect에서는 같은 플레이어 ID를 사용하고 새 Session을 같은 Actor에 binding하여 기존 플레이어 상태를 이어서 사용한다. 현재 위치는 Framework가 관리하므로 application route cache를 추가하거나 reconnect 때 플레이어 상태를 초기화하지 않는다. 처리 용량 때문에 호출을 수락하지 못하거나 대기시키는 동작이 backpressure이며, [Backpressure 가이드](https://docs.zlink.systems/ko/kotlin/guide/server/33-backpressure/)의 공개 결과를 관찰한다. 재접속한 앉은 플레이어는 새 좌석 없이 다시 받아들여지고, 현재 view와 카드가 다시 전송된다.

Framework 실패는 실제 reply·event·lifecycle 결과를 근거로 조사한다. Internal API·reflection·raw frame·사설 codec·추가 poller·고정 sleep·transport 재시도·deadline 확대·assertion 완화는 실패한 공개 동작을 검증하지 못하므로 해결 방법으로 추가하지 않는다. 코드 주석은 영어로 작성하고 실행·빌드 산출물인 `.logs/`, `build/`, `.gradle/`, `.kotlin/`과 license 파일(`zlink.license`)은 commit에서 제외한다. 프로젝트에는 formatter 설정이 없으므로 Kotlin 소스를 정리할 때는 이 디렉터리에서만 ktfmt `--kotlinlang-style`을 사용한다.

## 디버깅

### 실행별 로그

첫 실패 실행을 보존하고 로그를 읽은 뒤 재실행 여부를 결정한다. 간헐 실패도 실제로 관찰한 결함이며 이후 통과로 첫 실패를 제외하지 않는다. runner는 시작할 때 `Logs: <path>`를 출력한다. `.logs/run-<id>/`에는 `Nodes.kt`가 node 시작 시 작성한 실제 설정(`session.json`, `tableA.json`, `tableB.json`)과 node 시작마다 만드는 `<node>-<uuid>/` 디렉터리가 있으며, 그 안의 `<node>-stdout.log`는 준비·제어 줄을, `<node>-stderr.log`는 Framework 로그와 message flow를 기록한다. 교체한 node는 같은 설정 파일을 쓰고 새 디렉터리에서 시작한다.

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

`ZLINK_JAVA_STREAM_TRACE`는 공개 `0.30.2` package에 있는 버전별 진단 switch이므로 routing API로 사용하지 않는다. runner는 자신이 만든 process와 Compose project를 정리하고 Docker socket 제어 경로를 유지하며, 정리 오류도 보존하고 다른 작업의 process나 container는 중단하지 않는다.

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

제목에는 언어와 실패한 동작을 적는다(예: `framework(kotlin): Spot startup fails with empty SpotId`). 등록한 issue 번호는 README에 적되 Known issues 절이 없으면 새로 만들어 적어, 수정 package가 나오면 다시 검증할 위치를 남긴다.
