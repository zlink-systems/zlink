# 02. Topology discovery

[레퍼런스 목차](README.ko.md)

이 category는 `zlinkFramework()`(`ZLinkNestFrameworkOptionsBuilder`)가 제공하는 topology 등록
진입점과, RouteMesh·ClientServer·Fanout 운영 상태를 조회하는 진입점을 다룬다. 정확한 signature는
[기초 타입과 구성 exact interface](../../common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md),
[NestJS host adapter exact interface](../../common/spec/server/languages/node/interfaces/07-nestjs-host.ko.md)와
[Location 운영 조회와 observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.ko.md)가
소유한다. 등록 진입점은 모두 `zlinkFramework()` 체인 안의 구성 시점 호출이다.

---

## `addRouteMesh` (구성 시점)

물리 MeshNode 하나를 등록한다. RouteMesh 기반 topology의 시작점이다.

```ts
const play = zlinkFramework()
  .addRouteMesh("play")
  .listen(5501)
  .setRoutingIdPrefix("play")
  .setPlacementWeight(100);
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.listen(endpoint)` / `.listen(port?)` | 없으면 bind하지 않음 | 이 MeshNode의 수신 endpoint |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` | `configureNetwork()`의 root 기본값(BindHost `127.0.0.1`)을 따름 | 이 MeshNode에만 적용하는 bind·advertise host |
| `.routingId(routingId)` / `.setRoutingIdPrefix(prefix)` | Framework가 발급 | 고정 RID 또는 발급 RID의 prefix |
| `.setPlacementWeight(weight)` | 100(범위 `0..10000`) | 새 Actor·Spot을 이 node에 배치할 상대 가중치 |
| `.setActorLimit(limit)` / `.setSpotLimit(limit)` | Framework profile 기본값 | 이 node가 수용하는 Actor·Spot 상한 |
| `.setActivationConcurrency(limit)` | Framework 기본값 | activation admission 동시 실행 상한 |
| `.setDefaultRequestTimeout(timeoutMs)`(raw builder) | 이 MeshNode의 request 기본 timeout | messaging-execution category의 `requestToNode`/`requestToChannel`이 `.timeout(...)`을 생략했을 때 쓰는 값 |
| `.setInstanceSpotIdleTimeout(timeoutMs)` | `0`(정리하지 않음) | Instance Spot idle 회수 시간(ms) |
| `.configureRouterSocket()` | `ZLinkMeshNodeSocketConfig` 기본값 | 이 MeshNode ROUTER 소켓의 HWM·buffer·timeout(`maxMessageSize` 기본 `16_777_216` 등) |
| `.configureSpotPublisher()` | `ZLinkSpotPublisherConfig` 기본값 | Logical Multicast publisher socket의 HWM·timeout·linger |
| `.objects()` | — | Object role 등록으로 진입. Object role 등록 항목을 참고 |
| `.channel(channelName)` | — | 이 MeshNode의 RouteMesh Channel role 등록으로 진입. RouteMesh Channel 등록 항목을 참고 |
| `.peerConnections()` | — | Manual peer 연결 항목을 참고 |
| `.addSendHandler(packetName, handlerType)` / `.addRequestHandler(packetName, handlerType)`(NestJS builder) | — | Node direct handler 등록. `sendToNode`/`requestToNode`(messaging-execution category)가 호출하는 대상 |

**완료 결과.** 반환값 없이 동기로 등록된다. 잘못된 조합(중복 MeshName, listener 설정 누락 등)은
`ZLinkModule.forRoot(...)`이 초기화될 때 startup 검증에서 `ZLinkConfigurationException`으로
드러난다.

**선택 기준.** RouteMesh를 쓰는 모든 host가 최소 하나의 MeshNode를 등록할 때 쓴다. Manual peer만
쓰고 분산 discovery가 필요 없는 node는 Location Store 없이 시작할 수 있다.

---

## Object role 등록 (구성 시점)

MeshNode가 Actor·Spot을 어떻게 다루는지(Client만 하는지, Server로 호스팅하는지) 등록한다.

```ts
play.objects().server()
  .addEntrySpot(GameEntrySpot)
  .addSpotFactory("room", RoomSpot, (factory) => {
    factory.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    factory.preserveStateWith(RoomRelocationAdapter);
  })
  .addActorFactory("player", PlayerActorFactory, (factory) => {
    factory.preserveStateWith(PlayerRelocationAdapter);
  });
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.client()` / `.server()` | 생략하면 `None` | Object Client(참조만) 또는 Object Server(호스팅) 역할 선택. `server()`는 `client()` 기능을 포함하며 둘 다 Location Store가 필수다 |
| `.addEntrySpot(entrySpotType)` | 없음 | 외부 진입 전용 Entry Spot 타입 등록 |
| `.addSpotFactory(spotType, implementation, configure)` | 없음 | stable User Spot 타입 등록. `configure`는 `stableTypeLimit`·`executionMode`·`relocationReadiness`에 더해 `disableRelocation()`/`recreateOnRelocation()`/`preserveStateWith(...)` 중 정확히 하나를 호출해야 한다 |
| `.addInstanceSpotFactory(instanceSpotType, implementation, configure)` | 없음 | cold-activation Instance Spot 타입 등록. `configure`는 `stableTypeLimit`에 더해 relocation 동작 중 정확히 하나를 호출해야 한다 |
| `.addActorFactory(actorType, factoryType, configure)` | 없음 | stable Actor 타입 등록. `configure`는 relocation 동작 중 정확히 하나를 호출해야 한다(Actor factory에는 `stableTypeLimit`이 없다) |

**완료 결과.** 반환값 없이 동기로 등록된다. Relocation 동작을 생략하거나 둘 이상 호출하면 startup
configuration error다.

**선택 기준.** 이 node가 Actor·Spot을 실제로 호스팅(Server)하거나, 다른 node가 호스팅하는
Actor·Spot을 메시징 대상으로만 참조(Client)할 때 각각의 role을 등록한다. Relocation 정책 선택
기준은 actor-relocation category를 참고한다.

---

## RouteMesh Channel 등록 (구성 시점)

같은 MeshNode 안에서 논리 ChannelName membership을 등록한다.

```ts
play.channel("play.api").server()
  .setWeight(100)
  .addHandlerGroup("api");

play.channel("play.events").client();
```

**옵션.** `channel(channelName)` 뒤에는 `.client()` 또는 `.server()`를 정확히 한 번 호출한다.
`.client()`는 송신 경로만 만들고 modifier가 없다(TypeScript type 단계에서 잘못된 역할 설정을
막는다). `.server()`에 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.setWeight(weight)` | 100(범위 `0..10000`) | 이 Server가 request/send 대상으로 선택될 상대 가중치. `0`이면 선택 대상에서 제외 |
| `.addHandlerGroup(groupName)` | 없음 | `@ZlinkHandlerGroup(groupName)` decorator로 표시한 handler group 지정 |
| `.addSendHandler(handlerType)` / `.addRequestHandler(handlerType)`(raw builder) | packet name은 handler decorator에서 결정 | one-way·request/reply handler를 이 channel에 직접 등록 |
| `.addSendHandler(packetName, handlerType)` / `.addRequestHandler(packetName, handlerType)`(NestJS builder) | — | packet name을 명시하는 NestJS 표면의 동등한 등록 |

**완료 결과.** 반환값 없이 동기로 등록된다. 같은 owner의 handler key 중복은 startup 검증에서
`ZLinkConfigurationException`으로 드러난다.

**선택 기준.** `sendToChannel`/`requestToChannel`(messaging-execution category)로 받을 handler를
등록할 때 `.server()`를 쓴다. 이 MeshNode가 다른 node의 Server만 호출하고 자신은 handler를 두지
않으면 `.client()`만 등록한다. 서로 다른 프로세스 사이 통신이 필요하면 `addClientServerChannel`을
대신 쓴다.

---

## `addClientServerChannel` (구성 시점)

RouteMesh와 무관하게 독립된 ClientServer Channel을 등록한다.

```ts
zlinkFramework().addClientServerChannel("payments.api").server()
  .listen(6001)
  .setWeight(100)
  .addRequestHandler("charge", ChargeHandler);

zlinkFramework().addClientServerChannel("payments.api").client()
  .connect("payments-1:6001");
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.server().listen(port?)` | 자동 bind | 이 Server의 수신 port |
| `.server().setBindHost(host)` / `.setAdvertiseHost(host)` | root 기본값 | 이 Server에만 적용하는 bind·advertise host |
| `.server().setWeight(weight)` / `.addSendHandler`/`.addRequestHandler` | RouteMesh Channel Server와 동일 | 가중치와 handler 등록 |
| `.client().connect(endpoint)` | manual | 특정 Server에 수동 연결. 생략하면 automatic discovery로 target을 찾는다 |

**완료 결과.** 반환값 없이 동기로 등록된다. Automatic discovery를 쓰는 Client·Server는 Location
Store 등록이 없으면 startup 검증에서 configuration error로 드러난다. 같은 ChannelName에 Client와
Server를 각각 한 번씩 등록할 수 있지만 같은 역할을 두 번 등록하면 startup이 실패한다.

**선택 기준.** RouteMesh 멤버가 아닌 독립 서비스 사이의 request/reply나 one-way 메시징에 쓴다.
같은 RouteMesh 안 node끼리는 RouteMesh Channel 등록을 대신 쓴다.

---

## `addFanoutChannel` (구성 시점)

Classic fanout 전용 채널을 등록한다. `ZLinkFanoutClient.publish`(messaging-execution category)로
발행할 대상이다.

```ts
zlinkFramework().addFanoutChannel("lobby.events")
  .enablePublisher(7001)
  .addHandlerGroup("events");

// automatic subscriber — 같은 ChannelName의 publisher를 location store에서 자동으로 발견한다.
zlinkFramework().addFanoutChannel("lobby.events").enableSubscriber();

// manual subscriber — 명시한 endpoint만 쓴다.
zlinkFramework().addFanoutChannel("lobby.events").enableSubscriber("lobby-1:7001");
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.enablePublisher(endpoint)` / `.enablePublisher(port?)` | 없음 | 이 채널의 발행자 역할과 수신 endpoint 등록 |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` / `.routingId(rid)` / `.setRoutingIdPrefix(prefix)` | root 기본값 또는 Framework 발급 | publisher에만 적용하는 bind·advertise host와 RID |
| `.enableSubscriber()` (raw builder, endpoint 없음) | — | automatic subscriber. Location Store에서 같은 ChannelName의 유효한 publisher를 전부 찾는다 |
| `.enableSubscriber(endpoint)`(NestJS builder) / `.connect(endpoint)`(raw builder) | — | manual subscriber. 명시한 endpoint만 사용 |
| `.subscriberConnections()`(raw builder) | — | manual subscriber endpoint 집합의 runtime handle(`ZLinkEndpointConnections`: `connect`/`disconnect`/`listConnections`) 반환 |
| `.getListenerStatus(channelName)`(runtime, `ZLinkFanoutClient`) | — | publisher listener가 bind한 뒤 현재 advertised endpoint 조회 |

**완료 결과.** 반환값 없이 동기로 등록된다. Automatic subscriber와 manual subscriber를 같은
fanout channel에 함께 설정하면 startup 실패로 드러난다. `getListenerStatus(...)`는 host가
시작되지 않았거나 해당 channel이 publisher로 등록되지 않았으면 `ZLinkConfigurationException`으로
실패한다.

**선택 기준.** 발행자가 구독자를 알 필요가 없는 관찰·통지 채널을 새로 만들 때 쓴다. Reply가
필요한 메시징에는 RouteMesh Channel이나 ClientServer Channel 등록을 대신 쓴다.

---

## `addStreamNode` (구성 시점)

외부 STREAM 연결을 받는 listener를 등록한다.

```ts
zlinkFramework().addStreamNode("public-gateway")
  .bind(9001)
  .enableActorDispatch()
  .registerSession(GameSession);
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.bind(endpoint)` / `.bind(port?)` | 자동 bind | 이 STREAM listener의 수신 port |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` | root 기본값 | 이 listener에만 적용하는 bind·advertise host |
| `.setTlsServer(certPath, keyPath, requireClientCertificate?)` | TLS 없음 | TLS 서버 인증서·키, 상호 인증 여부 |
| `.enableActorDispatch()` | 비활성 | 수신 message를 global ActorId lookup으로 bound Actor에 dispatch |
| `.registerSession(sessionType)` | 없음 | `ZLinkSession`을 구현한 Session 타입(또는 `ZLinkSessionFactory`) 등록 |

**완료 결과.** 반환값 없이 동기로 등록된다. TLS 설정 오류는 startup 검증에서
`ZLinkConfigurationException`으로 드러난다.

**선택 기준.** 외부 client가 STREAM 프로토콜로 직접 연결하는 gateway를 열 때 쓴다. 정확한
Session·Actor 연결 규칙은 stream-session category를 참고한다.

---

## Manual peer 연결 (구성 시점·런타임)

Automatic discovery 없이 특정 endpoint에 수동으로 연결한다. `ZLinkMeshNodeBuilder.peerConnections()`로
호출한다.

```ts
play.peerConnections().connect("play-node-2:5501");
const connections = play.peerConnections().listConnections();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.connect(endpoint)` | expected RID 없음 | Admission handshake가 remote identity를 결정 |
| `.connect(expectedRoutingId, endpoint)` | — | Handshake identity가 다르면 admission하지 않음 |
| `.disconnect(endpoint)` | — | 등록한 연결을 해제 |
| `.listConnections()` | — | 현재 등록된 연결 목록 조회 |

**완료 결과.** 반환값 없이 동기로 등록·해제된다. 양쪽 MeshNode가 Object Client이고 둘 다
RouteMesh Channel Server membership이 없으면 이 연결 intent는 목록에 남아도 ready peer가 되지
않는다. 어느 한쪽에라도 weight `0`을 포함한 Channel Server membership이 있으면 연결을 만들고
liveness를 유지한다.

**선택 기준.** Automatic discovery(Location Store)를 쓰지 않고 고정된 peer 목록으로 RouteMesh를
구성할 때 쓴다.

---

## `filters` / `ZLinkHandlerFilter` (구성 시점)

모든 handler dispatch 앞에 공통 로직(인증, 로깅 등)을 끼워 넣는다. `zlinkFramework().options({...})`의
`filters` 배열로 등록한다.

```ts
zlinkFramework().options({
  filters: [AuthenticationFilter],
});

class AuthenticationFilter implements ZLinkHandlerFilter {
  async invoke(context: ZLinkHandlerFilterContext, next: ZLinkHandlerFilterNext) {
    if (!isAuthenticated(context)) {
      return; // next()를 호출하지 않으면 request는 Rejected로 끝난다
    }
    await next();
  }
}
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `options({ filters })` | 없음(등록한 순서대로 실행) | `ZLinkHandlerFilter` 구현 타입 배열을 dispatch 체인에 추가 |

**완료 결과.** 반환값 없이 동기로 등록된다. `next()`를 호출하면 남은 filter와 handler가
실행된다. Request에서 `next()`를 호출하지 않으면 `Rejected`로 끝나고, `next()`를 두 번 호출하면
`InvalidOperation`으로 실패하며 handler를 다시 실행하지 않는다. `context.dispatchKind`로
`NodeDirectSend`/`NodeDirectRequest`/`ChannelSend`/`ChannelRequest`/`ClassicFanout`을 구분한다 —
`ChannelSend`/`ChannelRequest`는 RouteMesh와 ClientServer를 모두 포함한다.

**선택 기준.** 개별 handler마다 반복할 공통 전처리·검증이 필요할 때 쓴다. Filter는 업무 reply를
직접 만들지 않는다 — 거부만 표현하고 나머지는 handler가 처리한다. Spot·Actor·Logical Multicast·
STREAM handler에는 적용하지 않는다.

---

## 기타 host-wide 옵션 (구성 시점)

`ZLinkNestFrameworkOptionsBuilder`가 제공하는 단순 값 하나로 끝나는 구성이다.

```ts
zlinkFramework()
  .configureNetwork()
  .bindHost = "0.0.0.0";

zlinkFramework()
  .configureInboundDispatch()
  .applicationHwmProfile(ZLinkApplicationHwmProfile.LowLatency);

zlinkFramework().configureStreamCompression().useLz4();
zlinkFramework().setApplicationVersion(2n);
zlinkFramework().options({ requestTimeoutMs: 30_000, worker: { minThreads: 2, maxThreads: 8 } });
```

**옵션.** 자주 쓰는 항목은 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.configureNetwork()` | `bindHost`는 `127.0.0.1` | 개별 listen 호출이 override하지 않는 한 쓰는 기본 bind·advertise host |
| `.configureInboundDispatch()` | `ZLinkApplicationHwmProfile.Balanced` | Inbound application HWM 크기·profile, process 메모리 상한 |
| `.configureDispatch()` | Framework 기본 정책 | Dispatch·diagnostics 옵션. observability-diagnostics category 참고 |
| `.configureStreamCompression()` | 압축 없음 | STREAM 기본 압축 codec(`useDefault()`/`useLz4()`/`use(codec)`/`disable()`) |
| `.setApplicationVersion(version)` / `.setMaintenanceWave(waveId)` | `0n` / 없음(exclusion 없음) | 모든 local MeshNode가 게시하는 배포 버전과 maintenance wave |
| `.options({ requestTimeoutMs, worker, dispatch, metrics, filters })` | 각 field 기본값 | host 전체 request timeout, worker pool, dispatch 옵션, metrics 연동, filter를 한 번에 지정 |
| `.codecs()` | JSON만 등록 | `zlinkFramework().codecs().use(extension)`. messaging-execution category의 Codec 등록 항목을 참고 |

**완료 결과.** 대부분 반환값 없이 동기로 실행되며, `.configureNetwork()`/`.configureInboundDispatch()`/
`.configureDispatch()`/`.codecs()`는 해당 builder나 options 객체를 반환해 그 위에서 추가 설정을
이어간다. 값 범위를 벗어나면 startup 검증에서 configuration error로 드러난다.

**선택 기준.** 위 전용 항목(host lifecycle·topology 등록·diagnostics)에 속하지 않는, 단순 값
하나로 끝나는 host-wide 설정을 조정할 때 쓴다.

---

## 런타임 weight 조회·변경

배포를 다시 하지 않고 placement weight나 channel weight를 바꾼다. `ZLINK_CHANNEL_RUNTIME_OPTIONS`
DI token으로 주입받는 `ZLinkRouteMeshRuntimeOptions`가 제공한다.

```ts
routeMeshRuntimeOptions.mesh("play").placementWeight = 50;
routeMeshRuntimeOptions.channel("play.api").weight = 0;
```

**옵션.** 이 진입점에는 두 개의 독립된 property가 있다.

| Property | 기본값 | 의미 |
| --- | --- | --- |
| `mesh(meshName).placementWeight` | 등록 시점 값 | node 단위 Actor·Spot 배치 가중치 |
| `channel(channelName).weight` | 등록 시점 값 | ChannelName 단위 Server 선택 가중치 |

**완료 결과.** 동기 get/set이다. 즉시 적용되며 별도 완료 신호가 없다.

**선택 기준.** 운영 중 배치나 트래픽 비중을 조정할 때 쓴다. `maxMessageSize`를 포함한 transport
option은 이 경로로 바꿀 수 없다 — startup 전에만 설정한다.

---

## Topology 상태 조회·관찰

RouteMesh·ClientServer·Fanout 각각의 운영 상태를 확인한다. 세 runtime(`ZLINK_ROUTE_MESH_RUNTIME`/
`ZLINK_CLIENT_SERVER_RUNTIME`/`ZLINK_FANOUT_RUNTIME` DI token)이 같은 모양(`snapshot` 한 번 조회,
`observe`로 스트리밍 관찰)을 제공한다.

```ts
const status = routeMeshRuntime.snapshot("play");
const canPlaceNewObjects = status.isReady && status.placement.isAvailable;

for await (const observed of routeMeshRuntime.observe("play", /*capacity=*/64)) {
  // observed.status.channels, observed.status.peers를 확인한다
}
```

**옵션.** 세 runtime의 대응 관계는 다음과 같다.

| Runtime | 대상 | 반환 status |
| --- | --- | --- |
| `ZLinkRouteMeshRuntime` | MeshName | `ZLinkRouteMeshStatus`(channels, peers, placement 포함) |
| `ZLinkClientServerRuntime` | ChannelName | `ZLinkClientServerStatus`(targets 포함) |
| `ZLinkFanoutRuntime` | ChannelName | `ZLinkFanoutStatus`(publishers 포함) |

**완료 결과.** `snapshot(...)`은 즉시 값을 반환하는 동기 호출이다. `observe(...)`는
`AsyncIterable<ZLinkObservedStatus<TStatus>>`를 반환하며, `loss` field로 관찰 유실 여부를
판단한다. 등록되지 않은 이름을 조회하면 새 상태를 만들지 않고 typed route error로 실패한다.

**선택 기준.** 특정 MeshName·ChannelName의 가용성을 판단하거나 장애 범위를 좁힐 때 쓴다. Host
전체 상태가 필요하면 host-lifecycle category의 `status`/`observe`를 쓴다.

---

전체 근거는
[기초 타입과 구성 exact interface](../../common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md),
[NestJS host adapter exact interface](../../common/spec/server/languages/node/interfaces/07-nestjs-host.ko.md)와
[Location 운영 조회와 observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.ko.md)를
참고한다.
