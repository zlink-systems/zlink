# Kotlin Tutorial

기능별 가이드가 코드를 읽어 가는 프로그램이다. 지금은 **Channel 메시징과 id로 부르는 Spot
하나**까지 담았다. Actor·STREAM은 아직 없다.

Kotlin은 이 저장소에 자기 디렉터리가 없다. Java 소스 옆
`framework/languages/java/` 아래에 산다. [`../../quickstart/`](../../quickstart/)가
`quickstart/{java,kotlin}`으로 나눠 둔 것과 같은 배치다.

## quickstart·샘플과 나눠 두는 이유

| | 목적 |
|---|---|
| [`../../quickstart/`](../../quickstart/) | 설치부터 첫 응답까지. 기능을 더하지 않는다 |
| **`tutorial/kotlin/`** (여기) | 기능을 차례로 쌓는다. 기능별 가이드가 이 코드를 읽는다 |
| [`../../samples/`](../../samples/) | 완결된 업무 흐름을 보이는 application |

## 배포된 패키지로 빌드한다

문서가 보여주는 코드와 독자가 Maven Central에서 받는 라이브러리를 같은 것으로 유지한다.
저장소 안에서 빌드해도 `systems.zlink:zlink-framework-*` 패키지를 참조하며, 이 디렉터리만
복사해 나가도 그대로 빌드된다. 버전은 [`../gradle/libs.versions.toml`](../gradle/libs.versions.toml)에 있다.

## 전제

- **JDK 25 이상.** `zlink-framework-core` 0.16.0의 class file 버전이 69(Java 25)다.
  이 프로그램은 Temurin 25.0.4.1로 빌드하고 실행했다.
- Gradle 9.3 (wrapper 포함).
- **Redis가 필요하다.** `127.0.0.1:6379`, 키 prefix는 `zlink-tutorial-kotlin:`다. Spot
  단계가 쓴다 — 방은 host가 아니라 id로 불리므로 지금 어느 node에 있는지를 Location Store에서
  읽는다. Channel 메시징만 볼 생각이면 Store 등록 두 자리와 `mesh.objects()` 호출을 빼면
  Redis 없이도 돈다. Fanout 구독자는 Store가 있어도 publisher endpoint를 직접 적는다
  (`Server`의 `fanout-subscribe` 참고).

### Windows에서 실행할 때

**별도 설정이 필요 없다.** `zlink-framework-core` 0.16.0의 POM이 binding
`systems.zlink:zlink:1.2.1`을 가리키고, 그 jar는 Windows native를 함께 싣는다.

```
$ unzip -l zlink-1.2.1.jar | grep native/
        0  1980-02-01 00:00   native/linux-x86_64/
  6211504  1980-02-01 00:00   native/linux-x86_64/libzlink.so.0
        0  1980-02-01 00:00   native/windows-x86_64/
  2275328  1980-02-01 00:00   native/windows-x86_64/zlink.dll
```

`LibraryLoader`가 jar 안에서 찾아 적재하므로 `ZLINK_LIBRARY_PATH`를 주지 않아도 된다.
1.1.0 jar에는 linux native만 들어 있어 Windows에서 `UnsatisfiedLinkError`가 났고, 그
수정(#465)이 binding 1.2.0부터 들어갔다.

`ZLINK_LIBRARY_PATH`로 다른 버전의 `zlink.dll`을 가리키면 기동은 되지만 호출에서 끊긴다.
jar 안의 것을 그대로 쓰면 이 문제가 없다.

```
java.lang.IllegalStateException: Missing native symbol 'zlink_publish'. Loaded libzlink is incompatible with this Java binding.
```

## 프로젝트

| 프로젝트 | 역할 |
|---|---|
| `Shared` | 두 쪽이 함께 쓰는 message 계약 |
| `Server` | channel handler와 filter를 실행하고 Spot 하나를 호스팅한다. 운영 endpoint 하나만 HTTP로 연다 |
| `Client` | HTTP를 받아 mesh로 호출한다 |
| `StreamClient` | mesh 밖의 client. framework가 아니라 connector 하나만 의존한다 |

## 포트

| 용도 | 포트 |
|---|---|
| Client HTTP | 5380 |
| Server HTTP (운영 endpoint) | 5381 |
| RouteMesh listen (Server) | 7601 |
| RouteMesh listen (Client) | 7602 |
| ClientServer (Server) | 7611 |
| Fanout publisher (Client) | 7612 |
| Stream node (Server) | 7621 |

## 실행

```bash
cd ..                      # tutorial/
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist

# 터미널 두 개. Server를 먼저 실행한다.
kotlin/Server/build/install/Server/bin/Server
kotlin/Client/build/install/Client/bin/Client
```

STREAM 단계의 외부 client는 세 번째 subproject다.

```bash
./gradlew :kotlin:StreamClient:installDist
kotlin/StreamClient/build/install/StreamClient/bin/StreamClient
```

Server가 peer를 받아들이면 이 줄이 찍힌다.

```
INFO 38136 --- [m-raw-mesh-game] s.z.f.r.binding.ZLinkJavaRawMeshNode     : ZLINK_FRAMEWORK_PEER_READY mesh=game peer=game-1926e18d-0ff9-4114-8168-2587672b7865
```

## 단계

각 기능은 따로 읽어도 된다.

### 1. Channel 메시징 — RouteMesh

요청하는 쪽이 node를 고르지 않는다. 채널 이름만 주면 그 채널을 담당하는 node가 받는다.

```bash
curl http://127.0.0.1:5380/players/p1/profile
# 200
# {"playerId":"p1","nickname":"rookie","level":1}

curl -i -X POST http://127.0.0.1:5380/players/p1/logins
# HTTP/1.1 202
```

두 번째는 응답을 기다리지 않는 단방향 호출이다. Server 로그에 남는다.

```
INFO 38136 --- [atcher-worker-1] s.z.t.server.channel.RecordLoginHandler  : login recorded: p1
```

### 2. Channel 메시징 — node 직접 호출

channel을 거치지 않는 경로다. 받는 쪽은 `mesh.addRouteRequestHandler`로 mesh에 바로
등록하고, 부르는 쪽은 node의 routing id를 지정한다. 운영 명령에만 쓴다.

```bash
curl http://127.0.0.1:5380/ops/nodes/game-server-1/status
# 200
# {"meshName":"game","channelName":"(none)","calledBy":"game-1926e18d-0ff9-4114-8168-2587672b7865","uptime":"31s","processId":38136}

curl -i http://127.0.0.1:5380/ops/nodes/no-such-node/status
# HTTP/1.1 404
# {"error":"not_found","message":"RouteMesh node request target was not found: no-such-node"}
# channel 호출과 달리 후보를 고르지 않으므로 그대로 실패한다.
# 404와 본문은 7절의 ZLinkErrorResponse가 낸 것이다.
```

`channelName`이 채널 이름이 아니라는 것이 요점이다. channel이 관여하지 않았다는 뜻이다.
`calledBy`는 부른 쪽 node의 routing id이고, 나머지 값은 답한 process 하나의 것이다.

이 호출에는 등록 쪽 조건이 둘 있다.

1. 받는 node가 `setRoutingId`로 id를 고정해야 한다. 고정하지 않으면 생성된 id라 부르는
   쪽이 적을 수 없다.
2. `peerConnections().connect(RoutingId, endpoint)`를 쓴다면, 받는 node가
   `setAdvertiseHost`로 **부르는 쪽이 적은 것과 같은 endpoint 문자열**을 알려야 한다.
   이 형태의 connect는 상대가 알리는 endpoint를 문자열 그대로 비교한다.
   `listen("tcp://0.0.0.0:7601")`만 해 두면 node는 `tcp://0.0.0.0:7601`을 알리는데 부르는
   쪽은 `tcp://127.0.0.1:7601`을 적으므로 peer가 거부된다.

`connect(RoutingId, endpoint)`는 **node 직접 호출의 전제가 아니다.** peer가 붙고 나면
`connect(endpoint)`만 쓴 client도 routing id로 그 node를 부를 수 있다. 확인한 결과다.

```bash
# client가 mesh.peerConnections().connect("tcp://127.0.0.1:7601")만 한 상태
curl http://127.0.0.1:5380/ops/nodes/game-server-1/status
# 200
# {"meshName":"game","channelName":"(none)","calledBy":"game-26a0e074-98ef-4f58-95f9-383855e5c7a2","uptime":"20s","processId":47652}
```

RoutingId를 적는 쪽은 "이 endpoint에는 이 node가 있어야 한다"는 기대를 거는 것이다.

2번을 빠뜨리면 호출이 이렇게 끝난다.

```
systems.zlink.framework.errors.ZLinkFrameworkException: RouteMesh node request route is not connected: game-server-1
```

이때 channel 호출도 같이 막힌다. peer가 서지 않아 후보가 없기 때문이다.

```
systems.zlink.framework.errors.ZLinkFrameworkException: one-way target was not found
```

### 3. Channel 메시징 — ClientServer

호출 코드는 위와 같다. 다른 것은 **누가 받느냐**다. 부르는 쪽이 연결한 서버가 받는다.

```bash
curl -i -X POST http://127.0.0.1:5380/players/p1/tickets
# HTTP/1.1 200
# ticket-p1
```

### 4. Channel 메시징 — Fanout

보내는 쪽이 받는 node를 모른다. 구독한 node가 모두 받는다.

```bash
curl -i -X POST http://127.0.0.1:5380/notices \
  -H 'Content-Type: application/json' -d '{"message":"scheduled maintenance"}'
# HTTP/1.1 202
```

Server 로그에 남는다.

```
INFO 38136 --- [atcher-worker-1] s.z.t.s.c.MaintenanceNoticeSubscriber    : maintenance notice: scheduled maintenance
```

### 5. Filter

등록한 filter가 이 node가 받는 handler를 감싼다. 위 네 호출을 모두 한 뒤의 Server 로그다.

```
INFO 38136 --- [  virtual-73277] s.z.t.server.dispatch.CallLogFilter      : dispatch start: GetPlayerProfile
INFO 38136 --- [atcher-worker-1] s.z.t.server.dispatch.CallLogFilter      : dispatch done: GetPlayerProfile in 0ms
INFO 38136 --- [  virtual-73480] s.z.t.server.dispatch.CallLogFilter      : dispatch start: RecordLogin
INFO 38136 --- [atcher-worker-1] s.z.t.server.channel.RecordLoginHandler  : login recorded: p1
INFO 38136 --- [atcher-worker-1] s.z.t.server.dispatch.CallLogFilter      : dispatch done: RecordLogin in 1ms
INFO 38136 --- [  virtual-73655] s.z.t.server.dispatch.CallLogFilter      : dispatch start: IssueSessionTicket
INFO 38136 --- [atcher-worker-1] s.z.t.server.dispatch.CallLogFilter      : dispatch done: IssueSessionTicket in 1ms
INFO 38136 --- [  virtual-74590] s.z.t.server.dispatch.CallLogFilter      : dispatch start: MaintenanceNotice
INFO 38136 --- [atcher-worker-1] s.z.t.s.c.MaintenanceNoticeSubscriber    : maintenance notice: scheduled maintenance
INFO 38136 --- [atcher-worker-1] s.z.t.server.dispatch.CallLogFilter      : dispatch done: MaintenanceNotice in 2ms
INFO 38136 --- [  virtual-74878] s.z.t.server.dispatch.CallLogFilter      : dispatch start: GetNodeStatus
INFO 38136 --- [atcher-worker-1] s.z.t.server.dispatch.CallLogFilter      : dispatch done: GetNodeStatus in 10ms
```

RouteMesh channel, ClientServer channel, classic fanout, node 직접 호출이 모두 걸린다.
Spot·Actor handler는 걸리지 않는다.

### 6. 실행 중 weight 바꾸기

등록은 기동 때 끝난다. **weight는 예외다.** 이 node가 돌면서 바꿀 수 있는 값이고, 바꾸면
다른 node가 새 호출의 후보로 이 node를 고르는지가 달라진다. `Server`가 5381을 여는 이유가
이것 하나다.

주입해 쓰는 것은 Java 표면 `ZLinkRouteMeshRuntimeOptions`다. Kotlin 전용 대응물은 없다.
`weight()`가 getter/setter 형태가 아니라 이름이 같은 method 둘이라 property로 쓰지 못하고
`mesh.channel(channel).weight(value)`처럼 호출로 쓴다.

0으로 내린다. socket은 그대로 열려 있고 처리 중이던 호출도 끝난다. 달라지는 것은 **새 호출의
후보에서 빠지는 것**이다.

```bash
curl -i -X POST 'http://127.0.0.1:5381/admin/channels/profile/weight?value=0'
# HTTP/1.1 200
# Content-Type: application/json
# {"channel":"profile","weight":0}
```

이 tutorial은 `profile` channel을 맡은 node가 이 하나뿐이다. 그래서 0으로 내리면 고를 후보가
남지 않아 호출이 그대로 실패한다.

```bash
curl -i http://127.0.0.1:5380/players/p1/profile
# HTTP/1.1 503
# Content-Type: application/json
# {"error":"unavailable","message":"one-way route is not connected"}
```

응답을 기다리지 않는 단방향 호출도 같다. 받을 후보가 없다는 것은 보내는 시점에 드러나므로
202가 아니라 같은 503이 온다.

```bash
curl -i -X POST http://127.0.0.1:5380/players/p1/logins
# HTTP/1.1 503
# Content-Type: application/json
# {"error":"unavailable","message":"one-way route is not connected"}
```

503과 이 본문은 `Client`에 둔 `ZLinkErrorResponse`가 낸 것이다. 7절에서 다룬다.

100으로 되돌리면 다시 받는다.

```bash
curl -i -X POST 'http://127.0.0.1:5381/admin/channels/profile/weight?value=100'
# HTTP/1.1 200
# {"channel":"profile","weight":100}

curl http://127.0.0.1:5380/players/p1/profile
# 200
# {"playerId":"p1","nickname":"rookie","level":1}
```

등록하지 않은 channel 이름을 주면 기동 때와 같은 검사에 걸린다.

```bash
curl -i -X POST 'http://127.0.0.1:5381/admin/channels/no-such-channel/weight?value=50'
# HTTP/1.1 500
# {"timestamp":"2026-09-16T19:03:42.624+00:00","status":500,"error":"Internal Server Error",
#  "path":"/admin/channels/no-such-channel/weight"}
```

`Server` 로그에 이유가 남는다.

```
systems.zlink.framework.errors.ZLinkConfigurationException: RouteMesh channel is not registered: no-such-channel
```

여기가 Spring Boot 기본 처리 그대로인 것은 7절의 매핑을 `Client`에만 두었기 때문이다.
`Server`가 여는 HTTP는 이 운영 endpoint 하나뿐이라 그쪽에는 두지 않았다.

weight를 100으로 되돌린 뒤 위 다섯 호출을 다시 해 보면 그대로 돈다. 확인한 결과다.

| 호출 | 결과 |
|---|---|
| `GET /players/p1/profile` | 200 `{"playerId":"p1","nickname":"rookie","level":1}` |
| `POST /players/p1/logins` | 202 |
| `GET /ops/nodes/game-server-1/status` | 200 `{"meshName":"game","channelName":"(none)",...}` |
| `POST /players/p1/tickets` | 200 `ticket-p1` |
| `POST /notices` | 202 |

### 7. 프레임워크 예외를 HTTP 상태코드로

framework 호출이 실패하면 `ZLinkFrameworkException`이 올라온다. 그대로 두면 Spring Boot의
기본 처리까지 가서 본문 없는 500 하나가 된다. 그러면 6절의 "지금 받을 node가 없다"와
handler 안의 결함이 호출자에게 같은 모양으로 보인다.

`Client`의 `ZLinkErrorResponse`가 `@RestControllerAdvice`·`@ExceptionHandler`로 이 예외를
받아 error kind를 상태코드로 옮긴다. endpoint마다 try/catch를 두지 않는다.

| ErrorKind | HTTP | 본문 `error` |
|---|---|---|
| `PROTOCOL_ERROR`·`TYPE_MISMATCH`·`INVALID_OPERATION` | 400 | `protocol_error`·`type_mismatch`·`invalid_operation` |
| `NOT_FOUND` | 404 | `not_found` |
| `ALREADY_EXISTS` | 409 | `already_exists` |
| `REJECTED` | 403 | `rejected` |
| `NOT_CONFIGURED`·`UNAVAILABLE`·`SHUTTING_DOWN` | 503 | `not_configured`·`unavailable`·`shutting_down` |
| `DEADLINE_EXCEEDED` | 504 | `deadline_exceeded` |
| `DATA_LOST`·그 밖 | 500 | `data_lost`·`internal_failure` |

본문은 이 형태다.

```json
{"error": "unavailable", "message": "one-way route is not connected"}
```

C++ tutorial에는 이 파일이 없다. C++ framework가 자체 HTTP host를 갖고 있어 같은 표가
framework 안에 있기 때문이다. Spring Boot·NestJS·ASP.NET 위에 올라가는 Kotlin·Java·Node·.NET
tutorial은 각자 이 매핑을 든다. 표는 네 판이 같다.

### 8. Spot — id로 부르기

지금까지의 호출은 모두 대상을 이름으로 골랐다. channel 이름을 주면 Framework가 그 channel을
맡은 node 중 하나를 고르고, routing id를 주면 그 node가 답했다. Spot은 다르다. **id 하나를
주면 그 id의 방이 지금 있는 node로 간다.**

```bash
curl -X POST http://127.0.0.1:5380/rooms   -H 'Content-Type: application/json' -d '{"title":"lobby"}'
# 7e4ad4fa-92d7-4043-8921-89b3df0112b0

curl -i -X POST http://127.0.0.1:5380/rooms/7e4ad4fa-92d7-4043-8921-89b3df0112b0/chat   -H 'Content-Type: application/json' -d '{"playerId":"p1","text":"hello"}'
# HTTP/1.1 202

curl http://127.0.0.1:5380/rooms/7e4ad4fa-92d7-4043-8921-89b3df0112b0
# {"title":"lobby","chat":["p1: hello"]}
```

id는 Framework가 만든다. 첫 응답에 따옴표가 없는 것은 ticket과 같은 이유다 — `String`을
그대로 돌려주면 Spring이 `text/plain`으로 쓴다. 두 번째 호출은 응답을 기다리지 않는
단방향이고, 세 번째는 방이 만든 답을 받는다. 방은 두 호출 사이에 상태를 들고 있었다.

Kotlin 쪽에서 알아 둘 것은 다음과 같다.

- **Spot 하나를 등록하는 순간 Location Store와 Relocation Store가 모두 필요하다.** 등록
  자체가 조건이라 relocation을 꺼도 Relocation Store를 요구한다.
- **방을 만드는 쪽에는 Kotlin 표면이 있고, 방을 부르는 쪽에는 없다.**
  `ZLinkSpotManager.kotlin()`은 `.await()`로 끝나지만, `sendToSpot`·`requestToSpot`은 Java
  표면을 그대로 쓰고 `kotlinx.coroutines.future.await`로 기다린다.
- **Kotlin user Spot은 admit할 actor 타입을 언제나 이름 짓는다.** 이 방은 actor를 받지
  않으므로 기반 타입 `ZLinkActor`를 적고 join을 모두 거절한다. .NET의 `IZLinkSpot`에는 그
  타입 인자가 없다.

### 9. Actor — id로 부르는 플레이어

방이 여럿이 함께 쓰는 자리라면 Actor는 개체 하나다. id를 **부르는 쪽이 정하고**, 같은 id로
다시 만들면 있던 것을 돌려준다.

```console
$ curl -X POST http://127.0.0.1:5380/players/p7 -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
created

$ curl -X POST http://127.0.0.1:5380/players/p7 -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
existing

$ curl http://127.0.0.1:5380/players/p7
{"playerId":"p7","nickname":"anonymous"}

$ curl -i -X POST http://127.0.0.1:5380/players/p7/nickname -H 'Content-Type: application/json' -d '{"nickname":"veteran"}'
HTTP/1.1 202

$ curl http://127.0.0.1:5380/players/p7
{"playerId":"p7","nickname":"veteran"}
```

Kotlin 쪽에서 알아 둘 것은 다음과 같다.

- **Actor는 생성자로 만들어지지 않는다.** `PlayerFactory`가 만들고, 의존성이 필요하면 거기서
  받는다.
- **actor handler에는 Kotlin 표면이 있다.** `ZLinkSuspendingEntrySpotActor*Handler`를 구현하면
  `suspend fun handle(...)`을 그대로 쓴다.
- **Actor를 만들고 부르는 쪽은 Java 표면이다.** `ZLinkActorManager`·`ZLinkActorClient`에는
  `kotlin()` wrapper가 없어 `kotlinx.coroutines.future.await`로 기다린다.

### 10. Location — 위치 조회

Spot과 Actor는 id로만 불렀고, 어디에 있는지는 Framework가 찾았다. 그 기록을 직접 읽는
호출이다.

```console
$ curl http://127.0.0.1:5380/locations/rooms/5613196d-2989-439f-89a1-ed19ad612184
{"spotId":"5613196d-2989-439f-89a1-ed19ad612184","node":"game-server-1"}

$ curl http://127.0.0.1:5380/locations/players/p7
{"actorId":"p7","node":"game-server-1"}

$ curl -i http://127.0.0.1:5380/locations/players/ghost
HTTP/1.1 404
```

조회는 Location Store만 읽고 대상에게는 아무것도 보내지 않는다. 지금 메시지를 받을 수 있는
대상만 답하므로, 만들어지는 중이거나 옮겨 가는 중이면 빈 값이 온다.

### 11. STREAM과 Session-Actor 연결

외부 client가 TCP로 붙는다. framework가 아니라 connector만 의존한다.

```console
$ kotlin/StreamClient/build/install/StreamClient/bin/StreamClient
connected: true
round trip: 339ms        # STREAM request/reply
bound player: p1         # 연결을 player에 묶는다
pushed: speedy           # player가 그 연결로 밀어 준다
```

`pushed`가 핵심이다. client는 nickname 변경만 보냈고, 응답이 아니라 **player가 스스로 민
알림**을 받았다.

Kotlin 쪽에서 알아 둘 것은 다음과 같다.

- **session에도 suspending 표면이 있다.** `ZLinkSuspendingSession`과
  `ZLinkSuspendingTypedSessionPacketHandler`를 쓴다.
- **suspending session handler는 타입으로 등록된다.** `addSessionPacketHandler(Class<*>)`가
  타입 인자를 묶지 않기 때문이다. channel handler의 handler group 우회가 여기서는 필요 없다.
- **`packetName()`을 직접 적는다.** Java의 session handler는 `messageType()` 하나로 끝난다.
- **`StreamClient`는 coroutine을 쓰지 않는다.** connector에 Kotlin wrapper가 없고 이 process는
  Spring도 없으므로, `CompletionStage`를 `join()`으로 기다린다.

## Kotlin 표면과 Java 표면

Kotlin 패키지 `zlink-framework-kotlin`은 Java 런타임 위에 얹히는 것이라, 이 프로그램도
두 표면을 섞어 쓴다.

### Kotlin 전용 표면을 쓴 곳

| 위치 | 쓴 것 |
|---|---|
| `Server` handler 넷 | `ZLinkSuspendingRequestHandler` / `ZLinkSuspendingSendHandler` / `ZLinkSuspendingPublishHandler` / `ZLinkSuspendingRouteRequestHandler` — 본문이 `suspend fun`이고 응답을 그대로 돌려준다 |
| `ServerApplication` | `options.useCoroutineHandlers(Dispatchers.Default)` — 위 handler를 부를 invoker를 단다 |
| `Client` 호출 넷 | `ZLinkRouteClient.kotlin()` / `ZLinkFanoutClient.kotlin()`으로 감싼 뒤 `requestToChannel<TReply>(...)`, `requestToNode<TReply>(...)`, `.await()` |

`requestToChannel<PlayerProfile>(...)`처럼 응답 타입이 reified 타입 인자다. Java 표면의
`.submit(PlayerProfile::class.java)`를 쓰지 않는다.

### Java 표면을 그대로 쓴 곳

| 위치 | 쓴 것 |
|---|---|
| 등록 전부 | `ZLinkFrameworkOptions`, `ZLinkMeshNodeBuilder`, `ClientServerChannelBuilder`, `FanoutChannelBuilder` |
| filter | `ZLinkHandlerFilter` — Kotlin filter interface는 없다. `suspend fun`이 아니라 `CompletionStage`를 받고 돌려준다 |
| handler context | `ZLinkMessageContext`, `ZLinkRouteMessageContext`, `ZLinkPublishMessageContext` |
| routing id | `systems.zlink.contracts.core.RoutingId` |
| 실행 중 weight | `ZLinkRouteMeshRuntimeOptions` — bean으로 주입받는다. Kotlin 대응물이 없고 `weight()`가 property도 아니다 |
| 예외 매핑 | `ZLinkFrameworkException`·`ZLinkFrameworkErrorKind` — Kotlin 대응물이 없다. `kind()`도 property가 아니라 호출로 쓴다 |

Kotlin DSL 확장(`routeMesh { }`, `channelName(name) { }`)도 있지만 이 프로그램은 쓰지 않는다.
Java builder를 그대로 이어 쓰는 쪽이 등록 순서를 읽기 쉬워서다.

### suspending handler는 타입으로 등록할 수 없다

Java builder의 `addRequestHandler`·`addSendHandler`는 타입 인자를
`THandler extends ZLinkRequestHandler<...>`로 묶는다. `ZLinkSuspendingRequestHandler`는
`ZLinkRequestHandler`를 구현하지 않으므로 이 자리에 넣을 수 없다. 그래서 RouteMesh channel과
ClientServer channel은 **handler group**으로 등록한다.

```kotlin
// 각 handler class에
@ZLinkHandlerGroup(HandlerGroups.PROFILE)

// 등록 쪽에
options.addHandlersFromPackageOf(GetPlayerProfileHandler::class.java)
mesh.channelName("profile").server().addHandlerGroup(HandlerGroups.PROFILE)
```

반대로 node 직접 호출과 fanout은 타입으로 등록한다. `ZLinkMeshNodeBuilder`에는
`addHandlerGroup`이 없고, `FanoutChannelBuilder.addPublishHandler(Class, Class)`는 타입
인자를 묶지 않기 때문이다.

### 묶인 연결이 없을 때의 push는 예외다

`boundSession()`에 묶인 연결이 없으면 이 binding은 **동기적으로 예외를 던진다.**
.NET·C++·Node에서 같은 호출은 아무 일도 하지 않고 끝난다. 그래서 `ChangeNicknameHandler`가
`runCatching`으로 그 실패를 잡는다 — 잡지 않으면 STREAM 없이 rename만 하는 9단계 호출이
handler 예외로 끝난다.

### 64비트 정수는 문자열로 간다

framework-json codec(`ZLinkFrameworkJsonProfile`)은 `Long`을 십진 JSON 문자열로 싣고
그렇게만 읽는다. `NodeStatus.processId`를 `Long`이 아니라 `Int`로 둔 것은 그래서다.

## 문서가 읽는 마커

문서는 코드를 손으로 옮겨 적지 않고 이 파일들에서 구간을 읽는다. 구간은 소스의 `--8<--`
마커가 정한다. 이름은 .NET·Java tutorial과 같게 두었다.

| 마커 | 자리 |
|---|---|
| `channel-contracts` | `Shared/.../Contracts.kt` |
| `clientserver-contracts` | `Shared/.../Contracts.kt` |
| `fanout-contracts` | `Shared/.../Contracts.kt` |
| `node-direct-contracts` | `Shared/.../Contracts.kt` |
| `channel-request-handler` | `Server/.../channel/GetPlayerProfileHandler.kt` |
| `channel-send-handler` | `Server/.../channel/RecordLoginHandler.kt` |
| `clientserver-handler` | `Server/.../channel/IssueSessionTicketHandler.kt` |
| `fanout-handler` | `Server/.../channel/MaintenanceNoticeSubscriber.kt` |
| `node-direct-handler` | `Server/.../ops/NodeStatusHandler.kt` |
| `filter-implementation` | `Server/.../dispatch/CallLogFilter.kt` |
| `coroutine-handlers` | `Server/.../ServerApplication.kt` |
| `filter-register` | `Server/.../ServerApplication.kt` |
| `mesh-register` | `Server/.../ServerApplication.kt` |
| `channel-register` | `Server/.../ServerApplication.kt` |
| `node-direct-register` | `Server/.../ServerApplication.kt` |
| `clientserver-register` | `Server/.../ServerApplication.kt` |
| `fanout-subscribe` | `Server/.../ServerApplication.kt` |
| `weight-runtime` | `Server/.../AdminEndpoints.kt` |
| `channel-client-register` | `Client/.../ClientApplication.kt` |
| `clientserver-client-register` | `Client/.../ClientApplication.kt` |
| `fanout-publish-register` | `Client/.../ClientApplication.kt` |
| `channel-request-call` | `Client/.../PlayerEndpoints.kt` |
| `channel-send-call` | `Client/.../PlayerEndpoints.kt` |
| `clientserver-call` | `Client/.../PlayerEndpoints.kt` |
| `node-direct-call` | `Client/.../OpsEndpoints.kt` |
| `fanout-call` | `Client/.../NoticeEndpoints.kt` |
| `spot-contracts` | `Shared/.../Contracts.kt` |
| `spot-class` · `spot-handlers` · `spot-handler-classes` | `Server/.../spots/GameRoom.kt` |
| `location-store` · `relocation-store` | `Server/.../ServerApplication.kt` |
| `object-server` · `spot-register` | `Server/.../ServerApplication.kt` |
| `location-store-client` · `spot-client-register` | `Client/.../ClientApplication.kt` |
| `spot-create-call` · `spot-message-call` | `Client/.../RoomEndpoints.kt` |
| `spot-send-call` · `spot-request-call` | `Client/.../RoomEndpoints.kt`. `spot-message-call` 안에 나뉘어 있다 |
| `location-find` | `Client/.../LocationEndpoints.kt` |
| `actor-contracts` | `Shared/.../Contracts.kt` |
| `actor-class` · `actor-factory` | `Server/.../actors/Player.kt` |
| `actor-handlers` · `actor-send-handler` · `actor-request-handler` · `actor-push` | `Server/.../actors/PlayerHandlers.kt` |
| `entry-spot` | `Server/.../spots/LobbySpot.kt` |
| `actor-register` | `Server/.../ServerApplication.kt` |
| `actor-create-call` · `actor-send-call` · `actor-request-call` | `Client/.../PlayerActorEndpoints.kt` |
| `stream-contracts` · `session-actor-contracts` | `Shared/.../Contracts.kt` |
| `session-class` · `session-actor-relay` | `Server/.../sessions/GameSession.kt` |
| `session-handler` · `session-actor-bind` | `Server/.../sessions/SessionHandlers.kt` |
| `stream-register` | `Server/.../ServerApplication.kt` |
| `stream-client` · `session-actor-client` | `StreamClient/.../StreamClientProgram.kt` |
| `error-mapping` | `Client/.../ZLinkErrorResponse.kt` |

마커 이름을 바꾸면 그 구간을 읽는 문서가 조용히 빈 코드 블록을 낸다. 이름을 바꿀 때는
문서를 함께 고친다.
