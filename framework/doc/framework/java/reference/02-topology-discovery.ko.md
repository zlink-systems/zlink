# 02. Topology discovery

[레퍼런스 목차](README.ko.md)

이 category는 `ZLinkFrameworkOptions`가 제공하는 topology 등록 진입점과, RouteMesh·ClientServer·Fanout
운영 상태를 조회하는 진입점을 다룬다. 정확한 signature는
[Java 구성과 host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.ko.md)와
[Java channel messaging exact interface](../../common/spec/server/languages/java/interfaces/channel-messaging.ko.md)가
소유한다. 등록 진입점은 모두 `ZLinkFrameworkConfigurer.configure(...)` 안에서 호출하는 구성 시점의
호출이다.

---

## `addRouteMesh` (구성 시점)

물리 MeshNode 하나를 등록한다. RouteMesh 기반 topology의 시작점이다.

```java
ZLinkMeshNodeBuilder play = options.addRouteMesh("play")
    .listen(5501)
    .setRoutingIdPrefix("play")
    .setPlacementWeight(100);
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.listen(endpoint)` / `.listen()` / `.listen(port)` | 없으면 bind하지 않음 | 이 MeshNode의 수신 endpoint |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` | `configureNetwork()`의 root 기본값(BindHost `127.0.0.1`)을 따름 | 이 MeshNode에만 적용하는 bind·advertise host |
| `.setRoutingId(routingId)` / `.setRoutingIdPrefix(prefix)` | Framework가 발급 | 고정 RID 또는 발급 RID의 prefix(ASCII `[A-Za-z0-9._-]` 1..64자) |
| `.setPlacementWeight(weight)` | 100(범위 `0..10000`) | 새 Actor·Spot을 이 node에 배치할 상대 가중치 |
| `.setActorCapacity(max)` / `.setSpotCapacity(max)` | active 10,000 / pending 128 | 이 node가 수용하는 Actor·Spot 상한 |
| `.setActivationConcurrency(max)` | Framework 기본값 | activation admission 동시 실행 상한 |
| `.setDefaultRequestTimeout(timeout)` | 이 MeshNode의 request 기본 timeout | messaging-execution category의 `requestToNode`/`requestToChannel`이 `.timeout(...)`을 생략했을 때 쓰는 값 |
| `.setInstanceSpotIdleTimeout(timeout)` | `Duration.ZERO`(정리하지 않음) | Instance Spot idle 회수 시간 |
| `.configureRouterSocket()` | `ZLinkMeshNodeSocketConfig` 기본값 | 이 MeshNode ROUTER 소켓의 HWM·buffer·timeout(`maxMessageSize` 기본 `16_777_216L` 등) |
| `.configureSpotPublisher()` | `ZLinkSpotPublisherConfig` 기본값 | Logical Multicast publisher socket의 HWM·timeout·linger |
| `.objects()` | — | Object role 등록으로 진입. Object role 등록 항목을 참고 |
| `.channel(channelName)` | — | 이 MeshNode의 RouteMesh Channel role 등록으로 진입. RouteMesh Channel 등록 항목을 참고 |
| `.peerConnections()` | — | Manual peer 연결 항목을 참고 |
| `.addRouteSendHandler(handlerType, messageType)` | packet name은 메시지 타입에서 결정 | Node direct one-way handler 등록. `sendToNode`(messaging-execution category)가 호출하는 대상 |
| `.addRouteRequestHandler(handlerType, requestType, replyType)` | packet name은 메시지 타입에서 결정 | Node direct request handler 등록. `requestToNode`가 호출하는 대상 |

**완료 결과.** 반환값 없이 동기로 등록된다. 잘못된 조합(중복 MeshName, listener 설정 누락 등)은
Spring context 초기화 시점의 startup 검증에서 `ZLinkConfigurationException`으로 드러난다.

**선택 기준.** RouteMesh를 쓰는 모든 host가 최소 하나의 MeshNode를 등록할 때 쓴다. Manual peer만
쓰고 분산 discovery가 필요 없는 node는 Location Store 없이 시작할 수 있다.

---

## Object role 등록 (구성 시점)

MeshNode가 Actor·Spot을 어떻게 다루는지(Client만 하는지, Server로 호스팅하는지) 등록한다.

```java
play.objects().server()
    .addEntrySpot(GameEntrySpot.class)
    .addSpotFactory("room", RoomSpot.class, factory -> factory
        .executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE)
        .preserveStateWith(RoomRelocationAdapter.class))
    .addActorFactory("player", PlayerActor.class, PlayerActorFactory.class, factory ->
        factory.preserveStateWith(PlayerRelocationAdapter.class));
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.client()` / `.server()` | — | Object Client(참조만) 또는 Object Server(호스팅) 역할 선택. `server()`는 `client()` 기능을 포함하며 둘 다 Location Store가 필수다. 생략하면 `None` |
| `.addEntrySpot(entrySpotClass)` | 없음 | 외부 진입 전용 Entry Spot 타입 등록 |
| `.addSpotFactory(spotType, spotClass, configure)` | 없음 | stable User Spot 타입 등록. `configure`는 `stableTypeLimit`·`executionMode`·`relocationReadiness`에 더해 `disableRelocation()`/`recreateOnRelocation()`/`preserveStateWith(...)` 중 정확히 하나를 호출해야 한다 |
| `.addInstanceSpotFactory(instanceSpotType, spotClass, configure)` | 없음 | cold-activation Instance Spot 타입 등록. `configure`는 `stableTypeLimit`에 더해 relocation 동작 중 정확히 하나를 호출해야 한다 |
| `.addActorFactory(actorType, actorClass, factoryClass, configure)` | 없음 | stable Actor 타입 등록. `configure`는 relocation 동작 중 정확히 하나를 호출해야 한다(Actor factory에는 `stableTypeLimit`이 없다) |

**완료 결과.** 반환값 없이 동기로 등록된다. Framework는 `configure` callback을 등록 호출 안에서
동기적으로 한 번만 실행한다 — 콜백이 반환된 뒤 보관한 builder를 다시 호출하면 configuration
error다. Relocation 동작을 생략하거나 둘 이상 호출하면 startup configuration error다.

**선택 기준.** 이 node가 Actor·Spot을 실제로 호스팅(Server)하거나, 다른 node가 호스팅하는
Actor·Spot을 메시징 대상으로만 참조(Client)할 때 각각의 role을 등록한다. Relocation 정책 선택
기준은 actor-relocation category를 참고한다.

---

## RouteMesh Channel 등록 (구성 시점)

같은 MeshNode 안에서 논리 ChannelName membership을 등록한다.

```java
play.channel("play.api").server()
    .setWeight(100)
    .addHandlerGroup("api")
    .addRequestHandler(GetPlayerHandler.class, GetPlayer.class, Player.class);

play.channel("play.events").client();
```

**옵션.** `channel(channelName)` 뒤에는 `.client()` 또는 `.server()`를 정확히 한 번 호출한다.
`.client()`는 송신 경로만 만들고 modifier가 없다. `.server()`에 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.setWeight(weight)` | 100(범위 `0..10000`) | 이 Server가 request/send 대상으로 선택될 상대 가중치. `0`이면 선택 대상에서 제외 |
| `.addHandlerGroup(groupName)` | 없음 | annotation 기반 handler scan이 찾는 handler group 지정 |
| `.addSendHandler(handlerType, messageType)` | packet name은 메시지 타입에서 결정 | one-way handler를 이 channel에 직접 등록 |
| `.addRequestHandler(handlerType, requestType, replyType)` | packet name은 메시지 타입에서 결정 | request/reply handler를 이 channel에 직접 등록 |

**완료 결과.** 반환값 없이 동기로 등록된다. 같은 owner의 handler key 중복은 startup 검증에서
`ZLinkConfigurationException`으로 드러난다.

**선택 기준.** `sendToChannel`/`requestToChannel`(messaging-execution category)로 받을 handler를
등록할 때 `.server()`를 쓴다. 이 MeshNode가 다른 node의 Server만 호출하고 자신은 handler를 두지
않으면 `.client()`만 등록한다. 서로 다른 프로세스 사이 통신이 필요하면 `addClientServerChannel`을
대신 쓴다.

---

## `addClientServerChannel` (구성 시점)

RouteMesh와 무관하게 독립된 ClientServer Channel을 등록한다.

```java
options.addClientServerChannel("payments.api").server()
    .listen(6001)
    .setWeight(100)
    .addRequestHandler(ChargeHandler.class, Charge.class, ChargeResult.class);

options.addClientServerChannel("payments.api").client()
    .connect("payments-1:6001");
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.server().listen()` / `.listen(port)` | 자동 bind | 이 Server의 수신 port |
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

```java
options.addFanoutChannel("lobby.events")
    .enablePublisher(7001)
    .addHandlerGroup("events");

// automatic subscriber — 같은 ChannelName의 publisher를 location store에서 자동으로 발견한다.
options.addFanoutChannel("lobby.events")
    .enableSubscriber();

// manual subscriber — 명시한 endpoint만 쓴다. enableSubscriber()와 함께 쓰면 startup이 실패한다.
options.addFanoutChannel("lobby.events")
    .connect("lobby-1:7001");
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.enablePublisher(endpoint)` / `.enablePublisher()` / `.enablePublisher(port)` | 없음 | 이 채널의 발행자 역할과 수신 endpoint 등록 |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` / `.setRoutingId(rid)` / `.setRoutingIdPrefix(prefix)` | root 기본값 또는 Framework 발급 | publisher에만 적용하는 bind·advertise host와 RID |
| `.enableSubscriber()` | — | automatic subscriber. Location Store에서 같은 ChannelName의 유효한 publisher를 전부 찾는다 |
| `.connect(endpoint)` | — | manual subscriber. 명시한 endpoint만 사용 |
| `.subscriberConnections()` | — | manual subscriber endpoint 집합의 runtime handle(`ZLinkEndpointConnections`: `connect`/`disconnect`/`listConnections`) 반환 |
| `.addHandlerGroup(groupName)` | 없음 | typed event handler group 연결 |

**완료 결과.** 반환값 없이 동기로 등록된다. Automatic subscriber와 manual subscriber를 같은
fanout channel에 함께 설정하면 startup 실패로 드러난다.

**선택 기준.** 발행자가 구독자를 알 필요가 없는 관찰·통지 채널을 새로 만들 때 쓴다. Reply가
필요한 메시징에는 RouteMesh Channel이나 ClientServer Channel 등록을 대신 쓴다.

---

## `addStreamNode` (구성 시점)

외부 STREAM 연결을 받는 listener를 등록한다.

```java
options.addStreamNode("public-gateway")
    .bind(9001)
    .enableActorDispatch()
    .registerSession(GameSession.class);
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.bind(endpoint)` / `.bind()` / `.bind(port)` | 자동 bind | 이 STREAM listener의 수신 port |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` | root 기본값 | 이 listener에만 적용하는 bind·advertise host |
| `.configureSocket()` | `ZLinkStreamSocketConfig` 기본값 | 이 listener 소켓의 `maxMessageSize` 등 세부 조정 |
| `.setTlsServer(certPath, keyPath)` / `.setTlsServer(certPath, keyPath, requireClientCertificate)` | TLS 없음 | TLS 서버 인증서·키, 상호 인증 여부 |
| `.enableActorDispatch()` | 비활성 | 수신 message를 global ActorId lookup으로 bound Actor에 dispatch |
| `.registerSession(sessionClass)` | 없음 | `ZLinkSession`을 구현한 Session 타입 등록 |
| `.addSessionPacketHandler(handlerType)` | 없음 | Session이 처리할 typed packet handler를 추가 등록 |

**완료 결과.** 반환값 없이 동기로 등록된다. TLS 설정 오류는 startup 검증에서
`ZLinkConfigurationException`으로 드러난다.

**선택 기준.** 외부 client가 STREAM 프로토콜로 직접 연결하는 gateway를 열 때 쓴다. 정확한
Session·Actor 연결 규칙은 stream-session category를 참고한다.

---

## Manual peer 연결 (구성 시점·런타임)

Automatic discovery 없이 특정 endpoint에 수동으로 연결한다. `ZLinkMeshNodeBuilder.peerConnections()`로
호출한다.

```java
play.peerConnections().connect("play-node-2:5501");
List<ZLinkMeshPeerConnection> connections = play.peerConnections().listConnections();
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
않는다. 어느 한쪽에라도 Channel Server membership이 있으면 weight가 `0`이어도 일반 peer
admission·liveness 규칙을 적용한다.

**선택 기준.** Automatic discovery(Location Store)를 쓰지 않고 고정된 peer 목록으로 RouteMesh를
구성할 때 쓴다.

---

## `useFilter` (구성 시점)

모든 handler dispatch 앞에 공통 로직(인증, 로깅 등)을 끼워 넣는다.

```java
options.useFilter(AuthenticationFilter.class);

public class AuthenticationFilter implements ZLinkHandlerFilter {
    @Override
    public <T> CompletionStage<T> invoke(
        ZLinkHandlerFilterContext context, ZLinkHandlerFilterNext<T> next) {
        if (!isAuthenticated(context)) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("rejected"));
        }
        return next.invoke();
    }
}
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.useFilter(filterType)` | 없음(등록한 순서대로 실행) | `ZLinkHandlerFilter` 구현체를 dispatch 체인에 추가 |

**완료 결과.** 반환값 없이 동기로 등록된다. `next.invoke()`를 호출하면 남은 filter와 handler가
실행된다. Request에서 `next`를 호출하지 않으면 `REJECTED`로 끝나고, 두 번 호출하면
`IllegalStateException`으로 거부하며 handler를 다시 실행하지 않는다.
`context.dispatchKind()`로 `NODE_DIRECT_SEND`/`NODE_DIRECT_REQUEST`/`CHANNEL_SEND`/
`CHANNEL_REQUEST`/`CLASSIC_FANOUT`을 구분한다.

**선택 기준.** 개별 handler마다 반복할 공통 전처리·검증이 필요할 때 쓴다. Filter는 업무 reply를
직접 만들지 않는다 — 거부만 표현하고 나머지는 handler가 처리한다. Spot·Actor·Logical Multicast·
STREAM handler에는 적용하지 않는다.

---

## 기타 host-wide 옵션 (구성 시점)

`ZLinkFrameworkOptions`가 제공하는 단순 값 하나로 끝나는 구성이다.

```java
options.addHandlersFromPackageOf(GameEntrySpot.class); // annotation으로 표시한 handler를 package에서 찾아 등록
options.configureNetwork().setBindHost("0.0.0.0");
options.configureInboundDispatch()
    .setApplicationHwmProfile(ZLinkApplicationHwmProfile.LOW_LATENCY);
options.configureMetadata()
    .allowSessionToActor("trace-id")
    .allowActorToSession("server-region");
options.configureStreamCompression().useLz4();
options.setApplicationVersion(2);
options.useVirtualThreadHandlers();
```

**옵션.** 자주 쓰는 항목은 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.addHandlersFromPackageOf(markerType)` | implicit auto-registration 활성 | annotation 기반 handler package scan 대상 지정 |
| `.configureMetadata().allowSessionToActor(key)` / `.allowActorToSession(key)` | 지정하지 않은 key는 forward 안 함 | STREAM session↔Actor relay로 넘길 metadata key를 방향별 allowlist에 추가 |
| `.configureNetwork()` | `bindHost()`는 `127.0.0.1` | 개별 listen 호출이 override하지 않는 한 쓰는 기본 bind·advertise host |
| `.configureWorkers()` | `ZLinkWorkerOptions` 기본값 | bounded worker pool의 최소·최대 thread 수, idle timeout, queue 상한 |
| `.configureInboundDispatch()` | `ZLinkApplicationHwmProfile.BALANCED` | Inbound application HWM 크기·profile, process 메모리 상한 |
| `.configureDispatch()` | Framework 기본 정책 | Dispatch·diagnostics 옵션. observability-diagnostics category 참고 |
| `.configureStreamCompression()` | 압축 없음 | STREAM 기본 압축 codec(`useDefault()`/`useLz4()`/`use(codec)`/`disable()`) |
| `.setApplicationVersion(version)` / `.setMaintenanceWave(wave)` | `0` / `null`(exclusion 없음) | 모든 local MeshNode가 게시하는 배포 버전과 maintenance wave |
| `.setDefaultRequestTimeout(timeout)` | Framework 기본값 | host 전체 request 기본 timeout |
| `.useVirtualThreadHandlers()` / `.useHandlerExecutor(executor)` | 구현 기본 executor | Handler dispatch에 쓸 실행 모델(virtual thread 또는 지정한 `Executor`) 선택. 상호 배타적이다 |
| `.codecs()` | JSON만 등록 | `options.codecs().use(extension)`. messaging-execution category의 Codec 등록 항목을 참고 |

**완료 결과.** 대부분 반환값 없이 동기로 실행되며, `.configureNetwork()`/`.configureWorkers()`/
`.configureInboundDispatch()`/`.configureDispatch()`/`.configureMetadata()`는 해당 builder나
options 객체를 반환해 그 위에서 추가 설정을 이어간다. 값 범위를 벗어나면 startup 검증에서
configuration error로 드러난다.

**선택 기준.** 위 전용 항목(host lifecycle·topology 등록·diagnostics)에 속하지 않는, 단순 값
하나로 끝나는 host-wide 설정을 조정할 때 쓴다.

---

## Topology 상태 조회·관찰

RouteMesh·ClientServer·Fanout 각각의 운영 상태를 확인한다. 세 runtime이 같은 모양(`snapshot`
한 번 조회, `observe`로 스트리밍 관찰)을 제공하며 모두 Spring bean으로 주입받는다.

```java
ZLinkMeshNodeSnapshot status = routeMeshRuntime.snapshot("play");
boolean canPlaceNewObjects = status.isReady() && status.placement().isAvailable();

routeMeshRuntime.observe("play", /*capacity=*/64)
    .subscribe(new Flow.Subscriber<>() { /* onNext(observed)에서 observed.status()를 확인 */ });
```

**옵션.** 세 runtime의 대응 관계는 다음과 같다.

| Runtime | 대상 | 반환 snapshot |
| --- | --- | --- |
| `ZLinkRouteMeshRuntime` | MeshName | `ZLinkMeshNodeSnapshot`(channels, peers, placement 포함) |
| `ZLinkClientServerRuntime` | ChannelName | `ZLinkClientServerStatus`(targets 포함) |
| `ZLinkFanoutRuntime` | ChannelName | `ZLinkFanoutStatus`(publishers 포함) |

**완료 결과.** `snapshot(...)`은 즉시 값을 반환하는 동기 호출이다. `observe(...)`는
`Flow.Publisher<ZLinkObservedStatus<TStatus>>`를 반환하며, `loss()` field로 관찰 유실 여부를
판단한다. `Flow.Publisher`는 `subscribe(...)` 뒤 `Subscription.request(n)`으로 수요를 알려야
값이 흐른다.

**선택 기준.** 특정 MeshName·ChannelName의 가용성을 판단하거나 장애 범위를 좁힐 때 쓴다. Host
전체 상태가 필요하면 host-lifecycle category의 `status`/`observe`를 쓴다.

---

전체 근거는
[Java 구성과 host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.ko.md)와
[Java channel messaging exact interface](../../common/spec/server/languages/java/interfaces/channel-messaging.ko.md)를
참고한다.
