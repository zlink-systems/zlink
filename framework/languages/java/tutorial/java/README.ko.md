# Java Tutorial — Channel 메시징과 Spot

`framework/languages/dotnet/tutorial/`의 Channel 메시징 부분과 id로 부르는 Spot 하나를
Java로 옮긴 것이다. Actor·STREAM은 담지 않았다. 문서가 읽는 `--8<--` 마커 이름은 .NET
쪽과 같게 두었다.

Kotlin tutorial이 같은 Gradle build 안 `../kotlin/`에 있다. 두 언어는 subproject만
나누어 쓰고 `settings.gradle.kts`·`gradle/libs.versions.toml`·wrapper는 함께 쓴다.

## 전제 조건

- **JDK 25.** 배포된 `zlink-framework-*`(버전은
  [`../gradle/libs.versions.toml`](../gradle/libs.versions.toml)의 `zlinkFramework` 참고)의
  class file major version이 69다. 이보다 낮은 JVM은 `UnsupportedClassVersionError`로 적재를
  거부한다. Gradle toolchain을 25로 고정해 두었고, **`installDist`로 만든 실행 script는
  `JAVA_HOME`을 그대로 쓰므로 실행 시점의 `JAVA_HOME`도 JDK 25여야 한다.**

  이 Gradle 프로젝트는 JDK를 자동으로 내려받는 toolchain resolver(예:
  `org.gradle.toolchains.foojay-resolver-convention`)를 설정하지 않는다. JDK 25가 없으면
  [Temurin 25](https://adoptium.net/)를 받아 설치하고 `JAVA_HOME`을 그 경로로 둔다(Gradle과
  `installDist` 실행 script 모두 `JAVA_HOME`을 toolchain·runtime 후보로 인식한다).

  ```bash
  # Linux/WSL
  export JAVA_HOME=/path/to/jdk-25.0.4.1+1
  ```

  ```powershell
  # Windows PowerShell
  $env:JAVA_HOME = "C:\path\to\jdk-25.0.4.1+1"
  ```

- **Docker Desktop.** Redis는 저장소 checkout 없이 Docker 컨테이너 하나로 띄운다(아래
  "빌드" 앞에 실행한다).

  ```bash
  docker run --rm -p 6379:6379 redis
  ```

이 밖에는 필요 없다. `zlink-framework-core`의 POM이 binding `systems.zlink:zlink`을
가리키고 그 jar가 Linux·Windows native를 함께 싣기 때문에(`LibraryLoader`가 jar 안에서 찾아
적재한다), Windows에서 별도 native 설정도 필요 없다.

## 내려받기와 설치

이 tutorial은 저장소를 checkout하지 않고 Maven Central의 `systems.zlink:zlink-framework-*`
패키지만 참조한다. 이 디렉터리(`java/`)만 옮겨도 그대로 빌드된다 — `settings.gradle.kts`·
`gradle/libs.versions.toml`·wrapper는 `../`(tutorial 루트)의 것을 함께 쓰므로 Kotlin
tutorial과 나란히 옮긴다. 별도로 내려받거나 설치할 것은 없다. Gradle wrapper(`./gradlew` /
`gradlew.bat`)가 첫 실행에서 정확한 Gradle 버전을 내려받고, 그 다음 Gradle이 위 패키지들을
Maven Central에서 내려받는다.

아래 명령은 모두 이 zip을 푼 디렉터리(`zlink-tutorial-java/`, tutorial 루트 - 여기서
`../`)에서 실행한다.

## 빌드

```bash title="linux"
./gradlew :java:Server:installDist :java:Client:installDist
```

```powershell title="windows"
.\gradlew.bat :java:Server:installDist :java:Client:installDist
```

## 실행

터미널 두 개. Server를 먼저 실행한다.

```bash title="linux"
./java/Server/build/install/Server/bin/Server
./java/Client/build/install/Client/bin/Client
```

```powershell title="windows"
.\java\Server\build\install\Server\bin\Server.bat
.\java\Client\build\install\Client\bin\Client.bat
```

STREAM 단계의 외부 client는 세 번째 subproject다.

```bash
./gradlew :java:StreamClient:installDist
./java/StreamClient/build/install/StreamClient/bin/StreamClient
```

## 검증

두 process가 서로를 받아들이면 양쪽 로그에 한 줄씩 남는다.

```
s.z.f.r.binding.ZLinkJavaRawMeshNode : ZLINK_FRAMEWORK_PEER_READY mesh=game peer=game-server-1
s.z.f.r.binding.ZLinkJavaRawMeshNode : ZLINK_FRAMEWORK_PEER_READY mesh=game peer=game-ee30c320-1220-452c-b1ea-22546350096c
```

Client가 뜬 뒤 아래 호출이 `200`과 함께 profile을 돌려주면 성공이다("단계"의 1번 참고).

```bash title="linux"
curl http://127.0.0.1:5280/players/p1/profile
# 200
# {"playerId":"p1","nickname":"rookie","level":1}
```

```powershell title="windows"
Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile'
# playerId nickname level
# -------- -------- -----
# p1       rookie   1
```

## 문제 해결

- **`UnsupportedClassVersionError`.**

  ```
  UnsupportedClassVersionError: systems/zlink/tutorial/server/ServerApplication
  has been compiled by a more recent version of the Java Runtime
  (class file version 69.0), this version of the Java Runtime only recognizes
  class file versions up to 66.0
  ```

  실행 시점의 `JAVA_HOME`이 JDK 25보다 낮다는 뜻이다. "전제 조건"대로 `JAVA_HOME`을 JDK 25로
  맞춘다.

- **`No matching toolchain found for requested specification: {languageVersion=25, ...}`.**
  빌드 머신에 JDK 25가 전혀 없다는 뜻이다. "전제 조건"의 Temurin 25 설치 절차를 따른다.

- **`Connection refused` (Redis, 6379).** Docker가 떠 있지 않거나 컨테이너가 아직
  기동 중이다. `docker run --rm -p 6379:6379 redis`를 다른 터미널에 띄워 두고, 로그에
  `Ready to accept connections`가 뜬 뒤 tutorial을 실행한다.

- **`Address already in use` (5280/5281/7501/7502/7511/7512/7521).** 이전 실행이 아직
  떠 있다. 두 process를 모두 종료한 뒤 다시 실행한다.

- **`ZLinkConfigurationException: MeshNode descriptor publication failed [mesh=game,
  status=REJECTED_CONFLICT]`, 또는 프로필 호출이 계속 `503 one-way route is not
  connected`를 낸다.** 같은 Redis를 다른 실행(다른 tutorial 시도, 이전에 비정상 종료한
  Server)이 먼저 써서 `zlink-tutorial-java:` 키 아래 mesh descriptor가 남아 있을 때 나온다.
  그 키만 지우고 Server부터 다시 실행한다 — 다른 언어의 tutorial(`zlink-tutorial-dotnet:`
  등)은 건드리지 않는다.

  ```bash
  redis-cli --scan --pattern 'zlink-tutorial-java:*' | xargs -r redis-cli del
  ```

## 구성

| Subproject | 역할 |
|---|---|
| `java/Shared` | 두 process가 함께 쓰는 message 계약 |
| `java/Server` | channel handler, node 직접 handler, fanout subscriber, filter, runtime weight endpoint, Spot 하나 |
| `java/Client` | HTTP를 받아 mesh·ClientServer·fanout으로 호출한다 |
| `java/StreamClient` | mesh 밖의 client. framework가 아니라 connector 하나만 의존한다 |

두 process 모두 `spring-boot-starter-web`을 쓴다. Client는 tutorial 호출을 받고, Server는
runtime weight endpoint 하나를 연다. Server도 HTTP를 여는 덕분에 embedded Tomcat이 JVM을
붙잡아 두므로 `SpringApplication#setKeepAlive(true)`는 더 이상 필요하지 않다.

두 subproject 모두 `-parameters`로 compile한다. 없으면 `@PathVariable String playerId`와
`@RequestParam int value`가 이름을 찾지 못해 모든 호출이 500이 된다.

## Port

| 쓰임 | 값 |
|---|---|
| Client HTTP | 5280 |
| Server HTTP (weight endpoint) | 5281 |
| mesh listen (Server) | `tcp://0.0.0.0:7501` |
| mesh listen (Client) | `tcp://0.0.0.0:7502` |
| ClientServer | 7511 |
| fanout publisher | `tcp://127.0.0.1:7512` |
| stream node (Server) | `tcp://0.0.0.0:7521` |

## 단계

각 기능은 따로 읽어도 된다.

### 1. Channel 메시징 — RouteMesh

요청하는 쪽이 node를 고르지 않는다. 채널 이름만 주면 그 채널을 담당하는 node가 받는다.

```bash
curl http://127.0.0.1:5280/players/p1/profile
# 200
# {"playerId":"p1","nickname":"rookie","level":1}

curl -i -X POST http://127.0.0.1:5280/players/p1/logins
# 202. Server 로그에 login recorded: p1
```

두 번째는 응답을 기다리지 않는 단방향 호출이다.

### 2. Channel 메시징 — node 직접 호출

channel을 거치지 않는 경로다. 받는 쪽은 `mesh.addRouteRequestHandler`로 mesh에 바로
등록하고, 부르는 쪽은 node의 routing id를 지정한다. 운영 명령에만 쓴다.

```bash
curl http://127.0.0.1:5280/ops/nodes/game-server-1/status
# 200
# {"meshName":"game","channelName":"(none)",
#  "calledBy":"game-ee30c320-1220-452c-b1ea-22546350096c","uptime":"35s","processId":46924}

curl -i http://127.0.0.1:5280/ops/nodes/no-such-node/status
# 404
# {"error":"not_found","message":"RouteMesh node request target was not found: no-such-node"}
```

channel 호출과 달리 후보를 고르지 않으므로 그대로 실패한다. 상태코드와 본문은 7번의
매핑이 정한다.

`channelName`이 비어 있는 것이 요점이다. channel이 관여하지 않았다는 뜻이다. `calledBy`는
부른 쪽 node의 routing id이고, 나머지 값은 답한 process 하나의 것이다. Client는 routing id를
고정하지 않았으므로 `game-` 뒤에 생성된 id가 붙는다.

이 호출에는 등록 쪽 조건이 셋 있다.

1. 받는 node가 `setRoutingId`로 id를 고정해야 한다. 고정하지 않으면 생성된 id라 부르는
   쪽이 URL에 적을 수 없다.
2. 받는 node가 `setAdvertiseHost`로 **부르는 쪽이 실제로 접속한 주소**를 알려야 한다.
   bind 주소 `0.0.0.0`을 그대로 광고하면 아래 3의 대조에서 어긋나 admission이 거부된다.
   거부는 조용하다. 로그에 남지 않고 `PEER_READY`만 영영 나오지 않는다.
3. 부르는 쪽이 `peerConnections().connect(RoutingId, endpoint)`로 어느 id가 그 endpoint에
   있는지 알려야 한다.

`connect(endpoint)`만 써도 node 직접 호출이 동작한다는 것을 확인했다. Java는 endpoint만
준 intent에 대해 `CONNECTION_READY` monitor edge가 알려 준 routing id를 쓰기 때문이다.
이 점은 .NET tutorial의 설명과 다르다.

### 3. Channel 메시징 — ClientServer

호출 코드는 위와 같다. 다른 것은 **누가 받느냐**다. 부르는 쪽이 연결한 서버가 받는다.

```bash
curl -i -X POST http://127.0.0.1:5280/players/p1/tickets
# 200
# ticket-p1
```

.NET은 같은 값을 JSON 문자열 `"ticket-p1"`로 낸다. Spring MVC는 `String` 반환값을
`text/plain`으로 쓰므로 따옴표가 없다. 값 자체는 같다.

### 4. Channel 메시징 — Fanout

보내는 쪽이 받는 node를 모른다. 구독한 node가 모두 받는다.

```bash
curl -i -X POST http://127.0.0.1:5280/notices \
  -H 'Content-Type: application/json' -d '{"message":"scheduled maintenance"}'
# 202. Server 로그에 maintenance notice: scheduled maintenance
```

### 5. Filter

Server에 `options.useFilter(CallLogFilter.class)` 하나만 등록했다. 위 호출을 모두 하면
Server 로그가 이렇게 남는다. handler마다 같은 logging을 되풀이하지 않는다.

```
s.z.t.server.dispatch.CallLogFilter      : dispatch start: GetPlayerProfile
s.z.t.server.dispatch.CallLogFilter      : dispatch done: GetPlayerProfile in 1ms
s.z.t.server.dispatch.CallLogFilter      : dispatch start: RecordLogin
s.z.t.server.channel.RecordLoginHandler  : login recorded: p1
s.z.t.server.dispatch.CallLogFilter      : dispatch done: RecordLogin in 2ms
s.z.t.server.dispatch.CallLogFilter      : dispatch start: IssueSessionTicket
s.z.t.server.dispatch.CallLogFilter      : dispatch done: IssueSessionTicket in 1ms
s.z.t.server.dispatch.CallLogFilter      : dispatch start: MaintenanceNotice
s.z.t.s.c.MaintenanceNoticeSubscriber    : maintenance notice: scheduled maintenance
s.z.t.server.dispatch.CallLogFilter      : dispatch done: MaintenanceNotice in 4ms
s.z.t.server.dispatch.CallLogFilter      : dispatch start: GetNodeStatus
s.z.t.server.dispatch.CallLogFilter      : dispatch done: GetNodeStatus in 2ms
```

RouteMesh channel, node 직접, ClientServer, fanout 네 경로가 모두 filter를 지난다.

### 6. Runtime weight 변경

node가 도는 중에 바꿀 수 있는 값은 weight 하나다. Server는 이 endpoint 때문에 HTTP를
연다. 포트 5281은 Client의 5280과 겹치지 않게 고른 것이다.

```bash
curl -s -X POST 'http://127.0.0.1:5281/admin/channels/profile/weight?value=0'
# 200
# {"channel":"profile","weight":0}
```

weight 0은 socket을 닫지 않는다. 진행 중인 일은 끝내되, 다른 node가 새 호출의 후보로 이
node를 고르지 않게 된다. 이 tutorial에는 `profile` channel을 담당하는 node가 하나뿐이라
0으로 두면 고를 후보가 남지 않고, 호출이 그대로 실패한다.

```bash
curl -i http://127.0.0.1:5280/players/p1/profile
# 503
# {"error":"unavailable","message":"one-way route is not connected"}

curl -i -X POST http://127.0.0.1:5280/players/p1/logins
# 503
# {"error":"unavailable","message":"one-way route is not connected"}
```

응답을 기다리지 않는 단방향 호출도 같은 값을 낸다. 보낼 후보가 없다는 것은 보내는
시점에 이미 드러나므로, 202를 내고 조용히 버리지 않는다.

request 호출 쪽은 Client 로그에 실패한 flow가 남는다. 실제로는 한 줄이고, 아래에서는
폭을 맞추려고 접었다. 단방향 호출은 이 줄을 남기지 않는다.

```
s.z.f.r.d.ZLinkMessageFlowTracer : zlink flow: event_id=zlink.message_flow
  phase=reply_received surface=channel kind=request channel=profile
  channel_route=route_mesh packet=GetPlayerProfile flow=01a0ab98-568c-7dfd-a912-7e274d6f0900
  origin=application outcome=failed
```

예외 자체는 로그에 stack trace로 남지 않는다. 7번의 advice가 잡아 응답으로 바꾸므로
Spring의 미처리 예외 logging까지 가지 않는다.

100으로 되돌리면 같은 호출이 다시 받는다.

```bash
curl -s -X POST 'http://127.0.0.1:5281/admin/channels/profile/weight?value=100'
# 200
# {"channel":"profile","weight":100}

curl -s http://127.0.0.1:5280/players/p1/profile
# 200
# {"playerId":"p1","nickname":"rookie","level":1}
```

등록되지 않은 channel 이름이나 범위 밖의 값을 주면 예외가 그대로 올라온다. 7번의 advice는
Client에만 두었고 이 endpoint는 Server가 받으므로, Spring의 기본 500이 나간다.
`ZLinkConfigurationException`은 `ZLinkFrameworkException`의 하위 type이라, 같은 advice를
Server에 두면 여기도 매핑을 탄다.

```bash
curl -s -o /dev/null -w '%{http_code}\n' \
  -X POST 'http://127.0.0.1:5281/admin/channels/no-such/weight?value=50'
# 500
# Server 로그:
# ZLinkConfigurationException: RouteMesh channel is not registered: no-such

curl -s -o /dev/null -w '%{http_code}\n' \
  -X POST 'http://127.0.0.1:5281/admin/channels/profile/weight?value=99999'
# 500
# Server 로그:
# ZLinkConfigurationException: weight must be in range 0..10000
```

이 호출은 filter를 지나지 않는다. Server가 직접 받는 HTTP이지 dispatch가 아니기 때문이다.

### 7. 예외 → HTTP 상태코드

Framework 호출은 `ZLinkFrameworkException`으로 실패한다. 그대로 두면 Spring의 기본 처리까지
올라가 모든 실패가 500이 되고, 부르는 쪽은 "지금 받을 node가 없다"와 "서버에 결함이 있다"를
구분하지 못한다. Client의 `ZLinkErrorResponse`가 `@RestControllerAdvice`로 error kind를
상태코드로 바꾼다. C++ tutorial은 framework의 HTTP host가 같은 표를 갖고 있어 이 파일이
필요 없다.

| ErrorKind | HTTP | 본문의 `error` |
|---|---|---|
| `PROTOCOL_ERROR` · `TYPE_MISMATCH` · `INVALID_OPERATION` | 400 | `protocol_error` · `type_mismatch` · `invalid_operation` |
| `NOT_FOUND` | 404 | `not_found` |
| `ALREADY_EXISTS` | 409 | `already_exists` |
| `REJECTED` | 403 | `rejected` |
| `NOT_CONFIGURED` · `UNAVAILABLE` · `SHUTTING_DOWN` | 503 | `not_configured` · `unavailable` · `shutting_down` |
| `DEADLINE_EXCEEDED` | 504 | `deadline_exceeded` |
| `DATA_LOST` · 그 밖 | 500 | `data_lost` · `internal_failure` |

본문은 `{"error": "<위 이름>", "message": "<예외 메시지>"}`이다. advice 하나가 controller
전체를 덮으므로 호출 자리마다 try/catch를 두지 않는다. `CompletionStage`를 돌려주는
endpoint도 함께 덮인다. Spring이 stage의 실패를 풀어서 handler에 넘기기 때문이다.

### 8. Spot — id로 부르기

지금까지의 호출은 모두 대상을 이름으로 골랐다. channel 이름을 주면 Framework가 그 channel을
맡은 node 중 하나를 고르고, routing id를 주면 그 node가 답했다. Spot은 다르다. **id 하나를
주면 그 id의 방이 지금 있는 node로 간다.**

```bash
curl -X POST http://127.0.0.1:5280/rooms   -H 'Content-Type: application/json' -d '{"title":"lobby"}'
# 97496c0a-5c0d-4447-8361-839354495f18

curl -i -X POST http://127.0.0.1:5280/rooms/97496c0a-5c0d-4447-8361-839354495f18/chat   -H 'Content-Type: application/json' -d '{"playerId":"p1","text":"hello"}'
# HTTP/1.1 202

curl http://127.0.0.1:5280/rooms/97496c0a-5c0d-4447-8361-839354495f18
# {"title":"lobby","chat":["p1: hello"]}
```

id는 Framework가 만든다. 첫 응답에 따옴표가 없는 것은 ticket과 같은 이유다 — `String`을
그대로 돌려주면 Spring이 `text/plain`으로 쓴다. 두 번째 호출은 응답을 기다리지 않는
단방향이고, 세 번째는 방이 만든 답을 받는다. 방은 두 호출 사이에 상태를 들고 있었다.

Java 쪽에서 알아 둘 것은 다음과 같다.

- **Spot 하나를 등록하는 순간 Location Store와 Relocation Store가 모두 필요하다.** 등록
  자체가 조건이라 relocation을 꺼도 Relocation Store를 요구한다.
- **Spot handler는 type 인자로 짝을 찾는다.** `ZLinkSpotPacketHandler<GameRoom, PostChat>`의
  두 인자가 대상 Spot과 payload이므로 packet 이름을 어디에도 적지 않는다. 등록은 Spot의
  생성자에서 `context.handlers().addHandler(...)`로 한다.
- **Java user Spot은 admit할 actor 타입을 언제나 이름 짓는다.** 이 방은 actor를 받지 않으므로
  기반 타입 `ZLinkActor`를 적고 join을 모두 거절한다. .NET의 `IZLinkSpot`에는 그 타입 인자가
  없다.

### 9. Actor — id로 부르는 플레이어

방이 여럿이 함께 쓰는 자리라면 Actor는 개체 하나다. id를 **부르는 쪽이 정하고**, 같은 id로
다시 만들면 있던 것을 돌려준다.

```console
$ curl -X POST http://127.0.0.1:5280/players/p7 -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
created

$ curl -X POST http://127.0.0.1:5280/players/p7 -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
existing

$ curl http://127.0.0.1:5280/players/p7
{"playerId":"p7","nickname":"anonymous"}

$ curl -i -X POST http://127.0.0.1:5280/players/p7/nickname -H 'Content-Type: application/json' -d '{"nickname":"veteran"}'
HTTP/1.1 202

$ curl http://127.0.0.1:5280/players/p7
{"playerId":"p7","nickname":"veteran"}
```

Java 쪽에서 알아 둘 것은 다음과 같다.

- **Actor는 생성자로 만들어지지 않는다.** `PlayerFactory`가 만들고, 의존성이 필요하면 거기서
  받는다.
- **Entry Spot을 하나 등록해야 한다.** 새로 만들어진 player가 처음 들어가는 자리다.
- **handler는 Spot과 Actor를 함께 받는다.** actor id로 보낸 메시지는 그 Actor가 지금 속한
  Spot 안에서 실행된다.

### 10. Location — 위치 조회

Spot과 Actor는 id로만 불렀고, 어디에 있는지는 Framework가 찾았다. 그 기록을 직접 읽는
호출이다.

```console
$ curl http://127.0.0.1:5280/locations/rooms/217c1a6a-313a-4e3e-bb18-e511c3feeefb
{"spotId":"217c1a6a-313a-4e3e-bb18-e511c3feeefb","node":"game-server-1"}

$ curl http://127.0.0.1:5280/locations/players/p7
{"actorId":"p7","node":"game-server-1"}

$ curl -i http://127.0.0.1:5280/locations/players/ghost
HTTP/1.1 404
```

조회는 Location Store만 읽고 대상에게는 아무것도 보내지 않는다. 지금 메시지를 받을 수 있는
대상만 답하므로, 만들어지는 중이거나 옮겨 가는 중이면 빈 값이 온다.

### 11. STREAM과 Session-Actor 연결

외부 client가 TCP로 붙는다. framework가 아니라 connector만 의존한다.

```console
$ ./java/StreamClient/build/install/StreamClient/bin/StreamClient
connected: true
round trip: 178ms        # STREAM request/reply
bound player: p1         # 연결을 player에 묶는다
pushed: speedy           # player가 그 연결로 밀어 준다
```

`pushed`가 핵심이다. client는 nickname 변경만 보냈고, 응답이 아니라 **player가 스스로 민
알림**을 받았다.

Java 쪽에서 알아 둘 것은 다음과 같다.

- **session handler는 stream node에 등록한다.** `addSessionPacketHandler(...)`가 그 자리이고,
  session class 안이 아니다. .NET과 Node는 session 쪽에서 등록한다.
- **`ZLinkSession`은 `onError`를 반드시 구현한다.** Node의 session callback은 모두 선택이다.
- **`client().reply`는 Request에만 답한다.** 기다리는 요청이 없는 client에 밀 때는 actor 쪽에서
  `boundSession().send(...)`를 쓴다.

## .NET과 표면이 다른 자리

| 자리 | .NET | Java |
|---|---|---|
| 계약 배치 | `Contracts.cs`에 record 여러 개 | 파일당 public type 하나라 `Contracts` class 안의 nested record. packet 이름은 simple name이라 양쪽이 같다 |
| handler 반환 | `ValueTask<T>` | `CompletionStage<T>` |
| handler 서명 | `HandleAsync(msg, ctx, CancellationToken)` | `handle(msg, ctx)`. Java 계약은 범용 cancellation token을 두지 않는다 |
| context 접근자 | `context.MeshName` (nullable) | `context.meshName()` (`Optional<String>`) |
| channel 선택 | `mesh.Channel("profile")` | `mesh.channelName("profile")` |
| fanout subscriber 등록 | `.EnableSubscriber().AddHandler<H, M>()` | `.connect(endpoint).addPublishHandler(H.class, M.class)`. 이름이 `addPublishHandler`지만 subscriber가 부를 handler를 등록하는 것이다 |
| fanout subscriber 자동 검색 | `EnableSubscriber()` + Location Store | `enableSubscriber()`가 같은 뜻이고, `connect(endpoint)`가 수동 쪽이다. 둘을 같이 쓰면 startup에서 거부된다 |
| 호출 종결 | `.Async<T>(ct)` / `.Async(ct)` | `.submit(T.class)` / `.submit()`. 동기 대안으로 `submit_sync`가 있다 |
| filter | `InvokeAsync(ctx, next, ct)`에서 `await next()` | `<T> invoke(ctx, ZLinkHandlerFilterNext<T>)`. stage를 돌려주므로 `await` 뒤 코드를 `whenComplete`로 붙인다 |
| handler 의존성 | 생성자로 `ILogger<T>` 주입 | handler instance를 Framework가 Spring으로 만든다. logger는 bean이 아니므로 static `LoggerFactory`를 쓴다 |
| node 직접 handler | `IZLinkRouteRequestHandler`, `ZLinkRouteMessageContext`(class) | `ZLinkRouteRequestHandler`, `ZLinkRouteMessageContext`(interface) |
| Server process | `WebApplication`이 살아 있게 한다 | weight endpoint 때문에 web starter를 쓰므로 embedded Tomcat이 같은 일을 한다. `setKeepAlive`는 쓰지 않는다 |
| runtime weight 접근 | endpoint 인자로 `IZLinkRouteMeshRuntimeOptions`를 받고 `mesh.Channel(c).Weight = v` | 같은 이름의 bean을 생성자로 주입하고 `mesh.channel(c).weight(v)`. 읽는 쪽은 `weight()`다. property가 아니라 이름이 같은 method 둘이다 |
| weight endpoint 입력 | minimal API가 `int value`를 query string에서 찾는다 | `@RequestParam int value`로 적는다 |
| weight 응답 | `Results.Ok(new { channel, weight })` 익명 type | nested record `WeightChanged(String channel, int weight)`. 본문 JSON은 같다 |

## 의미가 달라진 자리

- **fanout subscriber.** .NET tutorial은 Location Store가 publisher endpoint를 알려주는
  `EnableSubscriber()`를 쓴다. 여기는 Redis를 쓰지 않으므로 `connect("tcp://127.0.0.1:7512")`로
  endpoint를 손으로 준다. 주석이 설명하는 대비(자동 검색 대 수동 연결)는 그대로 살렸다.
- **mesh 광고 주소.** .NET tutorial은 `SetAdvertiseHost`를 mesh에 쓰지 않는다. Java는
  `connect(RoutingId, endpoint)`와 함께 쓸 때 이것이 없으면 admission이 성립하지 않는다.
- **ticket 응답의 표현.** 위 3번 참조. 값은 같고 media type이 다르다.
- **fanout publisher의 identity.** Location Store를 등록하면 publisher가 어느 identity로
  발행하는지 적어야 한다. Store가 publisher마다 행을 하나 두기 때문이다. Client의 fanout
  등록에 `setRoutingIdPrefix("game-client-broadcast")` 한 줄이 그것이고, Store가 없던
  단계에서는 없던 줄이다.

- **묶인 연결이 없을 때의 push.** `boundSession()`에 묶인 연결이 없으면 이 binding은
  **동기적으로 예외를 던진다.** .NET·C++·Node에서 같은 호출은 아무 일도 하지 않고 끝난다.
  그래서 `ChangeNickname` handler가 그 실패를 잡아 버린다 — 잡지 않으면 STREAM 없이 rename만
  하는 8단계 호출이 handler 예외로 끝난다.

## 문서가 읽는 마커

| 마커 | 자리 |
|---|---|
| `channel-contracts` | `Shared/.../Contracts.java` |
| `clientserver-contracts` | `Shared/.../Contracts.java` |
| `fanout-contracts` | `Shared/.../Contracts.java` |
| `node-direct-contracts` | `Shared/.../Contracts.java` |
| `channel-request-handler` | `Server/.../channel/GetPlayerProfileHandler.java` |
| `channel-send-handler` | `Server/.../channel/RecordLoginHandler.java` |
| `clientserver-handler` | `Server/.../channel/IssueSessionTicketHandler.java` |
| `fanout-handler` | `Server/.../channel/MaintenanceNoticeSubscriber.java` |
| `node-direct-handler` | `Server/.../ops/NodeStatusHandler.java` |
| `filter-implementation` | `Server/.../dispatch/CallLogFilter.java` |
| `filter-register` | `Server/.../ServerApplication.java` |
| `mesh-register` | `Server/.../ServerApplication.java` |
| `channel-register` | `Server/.../ServerApplication.java` |
| `node-direct-register` | `Server/.../ServerApplication.java` |
| `spot-contracts` | `Shared/.../Contracts.java` |
| `spot-class` · `spot-handlers` | `Server/.../spots/GameRoom.java` |
| `location-store` · `relocation-store` | `Server/.../ServerApplication.java` |
| `object-server` · `spot-register` | `Server/.../ServerApplication.java` |
| `location-store-client` · `spot-client-register` | `Client/.../ClientApplication.java` |
| `spot-create-call` · `spot-message-call` | `Client/.../ClientApplication.java` |
| `spot-send-call` · `spot-request-call` | `Client/.../ClientApplication.java`. `spot-message-call` 안에 나뉘어 있다 |
| `clientserver-register` | `Server/.../ServerApplication.java` |
| `fanout-subscribe` | `Server/.../ServerApplication.java` |
| `channel-client-register` | `Client/.../ClientApplication.java` |
| `clientserver-client-register` | `Client/.../ClientApplication.java` |
| `fanout-publish-register` | `Client/.../ClientApplication.java` |
| `channel-request-call` | `Client/.../ClientApplication.java` |
| `channel-send-call` | `Client/.../ClientApplication.java` |
| `node-direct-call` | `Client/.../ClientApplication.java` |
| `clientserver-call` | `Client/.../ClientApplication.java` |
| `fanout-call` | `Client/.../ClientApplication.java` |
| `weight-runtime` | `Server/.../ServerApplication.java` |
| `location-find` | `Client/.../ClientApplication.java` |
| `actor-contracts` | `Shared/.../Contracts.java` |
| `actor-class` | `Server/.../actors/Player.java` |
| `actor-factory` | `Server/.../actors/PlayerFactory.java` |
| `actor-send-handler` · `actor-push` | `Server/.../actors/ChangeNicknameHandler.java` |
| `actor-request-handler` | `Server/.../actors/GetPlayerHandler.java` |
| `entry-spot` | `Server/.../spots/LobbySpot.java` |
| `actor-register` | `Server/.../ServerApplication.java` |
| `actor-create-call` · `actor-send-call` · `actor-request-call` | `Client/.../ClientApplication.java` |
| `stream-contracts` · `session-actor-contracts` | `Shared/.../Contracts.java` |
| `session-class` · `session-actor-relay` | `Server/.../sessions/GameSession.java` |
| `session-handler` | `Server/.../sessions/PingHandler.java` |
| `session-actor-bind` | `Server/.../sessions/AuthenticateHandler.java` |
| `stream-register` | `Server/.../ServerApplication.java` |
| `stream-client` · `session-actor-client` | `StreamClient/.../StreamClientProgram.java` |
| `error-mapping` | `Client/.../ZLinkErrorResponse.java` |

마커 이름을 바꾸면 그 구간을 읽는 문서가 조용히 빈 코드 블록을 낸다. 이름을 바꿀 때는
문서를 함께 고친다.
