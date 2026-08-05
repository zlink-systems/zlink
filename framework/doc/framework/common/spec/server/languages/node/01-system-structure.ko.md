<!-- framework-adapter-nav:start -->
[문서 목록](../../../../../../README.ko.md)
<!-- framework-adapter-nav:end -->

[Node spec 목차](README.ko.md)

# Node 시스템 구조 — 패키지, 등록과 부트스트랩

> 이 문서는 **NestJS 위에서 ZLink framework를 어떻게 구성하는가**를 소유한다. 패키지 구조, 배포,
> 모듈 부트스트랩, DI, lifecycle, 그리고 각 기능의 **등록 표면**이다.
>
> **기능의 의미와 동작 규칙은 공통 스펙이 소유한다** — [channel-messaging](../../../08-channel-messaging.ko.md),
> [spot-messaging](../../../12-spot-messaging.ko.md), [MeshNode](../../../13-mesh-node.ko.md),
> [stream-session](../../../19-stream-session.ko.md), [actor-model](../../../14-actor-model.ko.md),
> [session-actor-dispatch](../../../20-session-actor-dispatch.ko.md),
> [runtime-monitoring](../../../24-runtime-monitoring.ko.md),
> [location-runtime](../../../21-location-runtime.ko.md),
> [channel-topology](../../../07-channel-topology.ko.md).
>
> **public 타입과 시그니처는 [인터페이스 목차](interfaces/README.ko.md)가 범주별로 소유한다.**
> 이 문서는 Node framework의 시스템 구조와 package 경계만 정의하며 사용 예제와 튜토리얼은 포함하지 않는다.
> client connector는 [stream-connector](../../../stream-connector/languages/typescript/03-stream-connector.ko.md)가 소유한다.

## 1. 패키지 구조

| package | 역할 | 의존 |
|---|---|---|
| `@zlink-systems/framework` | framework core — contract, runtime, dispatcher | `zlink`, `stream-wire`, OpenTelemetry API |
| `@zlink-systems/nestjs` | NestJS host adapter — `ZLinkModule.forRoot(...)` 등록 표면 | `framework`, NestJS common/core, `reflect-metadata`, `rxjs` |
| `@zlink-systems/framework-codec-protobuf` | Protobuf codec **extension** | `framework`, `stream-connector`, `protobufjs` |
| `@zlink-systems/framework-codec-msgpack` | MessagePack codec **extension** | `framework`, `stream-connector`, `@msgpack/msgpack` |
| `@zlink-systems/framework-locations-redis` | Redis location store **extension** | `framework`, `zlink`, `redis` |
| `@zlink-systems/http-client` | fluent HTTP/JSON client | `framework`, `undici` |
| `@zlink-systems/stream-connector` | **client** connector — 서버 framework에 의존하지 않는다 | `stream-wire` |
| `@zlink-systems/stream-wire` | connector와 서버가 공유하는 **wire 계층** | 없음 |

**분리 원칙:**

- **codec 구현을 core에 섞지 않는다.** JSON은 기본 codec이고, Protobuf·MessagePack은
  extension package로 분리한다. 현재 Node HTTP client는 framework codec registry를
  사용하지 않는다. Codec 공유 범위의 공통 계약은
  [Framework API §9](../../../06-framework-api.ko.md#9-codec)을 따른다.
- **[location store](../../../01-glossary.ko.md#location-store) 구현도 extension이다.** core는 store 계약만 알고 Redis 구현은 별도 package가
  제공한다(§10).
- **connector는 서버 framework package를 참조하지 않는다.** 반대 방향도 같다.
- **`stream-wire`는 환경 중립이다.** `Uint8Array`만 사용하고 `Buffer`에 의존하지 않으므로 Node와
  브라우저에서 **같은 코드**로 동작한다.
- **host adapter(`nestjs`)와 core를 나눈다.** core는 NestJS에 의존하지 않는다.

## 2. 배포 계약

| package | 배포 채널 | 소비자 |
|---|---|---|
| `@zlink-systems/framework` · `@zlink-systems/nestjs` | npm | 서버 애플리케이션 |
| `@zlink-systems/framework-codec-*` | npm | codec이 필요한 서버·브라우저 client |
| `@zlink-systems/framework-locations-redis` | npm | 다중 프로세스 배포 |
| `@zlink-systems/stream-connector` | npm | 브라우저 계열 client |
| `@zlink-systems/stream-wire` | npm | connector와 서버가 공유 |

**TypeScript connector는 package root 하나를 ESM으로 배포한다.** 이 진입점은 브라우저 계열
client에서 플랫폼 `WebSocket`을 사용한다. Node.js에서 실행하는 connector와 별도 browser
subpath는 제공하지 않는다. 정확한 계약은
[TypeScript Stream Connector](../../../stream-connector/languages/typescript/03-stream-connector.ko.md)가 소유한다.

## 3. 모듈 부트스트랩

`ZLinkModule.forRoot(...)` / `forRootFactory(...)`가 등록 진입점이다.

**`forRoot(...)`은 transport·node·역할·handler group 선택을 선언하는 자리다. application 객체
그래프를 조립하는 자리가 아니다.**

## 4. DI

- framework가 노출하는 outbound client와 manager는 **NestJS provider token**으로 등록한다.
  `@Inject(TOKEN)`으로 받으며 token은 framework가 export한다.
- **handler는 context의 service locator가 아니라 생성자 주입으로 의존을 받는다.**
  **context에 DI 컨테이너를 넣지 않는다.**
- application이 구현하는 객체는 **NestJS DI 컨테이너가 소유한다.** 부트스트랩 코드에서 직접
  `new`로 만들지 않고 module `providers`에 등록한다.

| 객체 | 등록 | framework가 resolve하는 시점 |
|---|---|---|
| channel/fanout/route handler | `providers` + handler 등록 표면 | channel이 그 handler group을 dispatch할 때 |
| Entry Spot, user Spot | `providers` + `addEntrySpot(...)` / `addSpotFactory(...)` | MeshNode·SpotManager가 local Spot을 활성화할 때 |
| Instance Spot | `providers` + `addInstanceSpotFactory(...)` | Spot direct fluent call이 Instance cold activation을 시작할 때 |
| [Spot](../../../01-glossary.ko.md#spot) packet·subscribe·actor·timer handler | handler decorator + `zlinkDiscoverProviders(...)` | 그 Spot 실행 문맥에서 처리할 때 |
| actor factory | `providers` + `addActorFactory(...)` | ActorManager가 actor를 생성할 때 |
| stream session(또는 [factory](../../../01-glossary.ko.md#factory)) | `providers` + `streams` 설정 | stream 연결을 session으로 활성화할 때 |

### 4.1 Provider token

**주입에 쓰는 token 심볼은 `@zlink-systems/nestjs` package root가 export한다.**

**항상 등록되는 provider:**

| token | 표면 |
|---|---|
| `ZLINK_CHANNEL_CLIENT` | channel client |
| `ZLINK_ROUTE_CLIENT` | route client |
| `ZLINK_FANOUT_CLIENT` | fanout client |
| `ZLINK_BOUND_SESSION_FACTORY` | bound session factory |
| `ZLINK_CHANNEL_RUNTIME_OPTIONS` | channel runtime options |
| `ZLinkDrainHealthIndicator` | [MeshNode](../../../01-glossary.ko.md#meshnode) readiness와 health indicator |
| `ZLINK_MESSAGE_METADATA_POLICY` | metadata 정책 |
| `ZLINK_FRAMEWORK_RUNTIME` · `ZLINK_FRAMEWORK_REGISTRATION` | runtime과 등록 |

**역할이 있을 때만 등록되는 provider:**

| token | 필요한 역할 |
|---|---|
| `ZLINK_SPOT_MANAGER` · `ZLINK_SPOT_OUTBOUND` | MeshNode의 Spot 등록 |
| `ZLINK_SPOT_PUBLISHER_CLIENT` | spot publisher 역할 |
| `ZLINK_ACTOR_CLIENT` | MeshNode와 location store가 모두 등록됨 |
| `ZLINK_ACTOR_MANAGER` | actor manager가 활성화됨 |
| `ZLINK_LOCATION_RUNTIME_QUERY` | location store가 하나 이상 등록됨 |
| `ZLINK_ROUTE_MESH_RUNTIME` | RouteMesh MeshNode가 하나 이상 등록됨 |
| `ZLINK_CLIENT_SERVER_RUNTIME` | ClientServer Channel이 하나 이상 등록됨 |
| `ZLINK_FANOUT_RUNTIME` | endpoint 없는 automatic fanout subscriber가 하나 이상 등록됨 |

**등록되지 않은 token을 주입하면 NestJS의 미해결 의존성 오류로 실패한다.**

`@zlink-systems/framework` package root는 host 단위 `ZLinkFrameworkRuntime`,
`ZLinkRouteMeshRuntime`, `ZLinkClientServerRuntime`과
`ZLinkFanoutRuntime` interface를 export한다. `@zlink-systems/nestjs` package root는 각각에 대응하는
`ZLINK_ROUTE_MESH_RUNTIME`, `ZLINK_CLIENT_SERVER_RUNTIME`, `ZLINK_FANOUT_RUNTIME` token을 export한다.
NestJS의 `ZLinkModule`은 위 표의 등록 조건을 만족하는 runtime instance를 해당 token의 provider로 등록하고
dynamic module 밖에서도 주입할 수 있도록 provider를 export한다.

정적 `forRoot`에서 [RouteMesh](../../../01-glossary.ko.md#routemesh) MeshNode가 없으면 RouteMesh runtime provider를, [ClientServer Channel](../../../01-glossary.ko.md#clientserver-channel)이 없으면
ClientServer runtime provider를 만들지 않는다. Manual fanout subscriber만 있으면 fanout runtime provider를
만들지 않는다. `forRootFactory`에서 구성을 동적으로 정하는 경우에는 아래 공통 규칙대로 각 조건부 provider 값이
`null`일 수 있다. Application은 다음 token으로 public monitoring interface만 주입받는다.

```ts
class MonitoringProbe {
    constructor(
        @Inject(ZLINK_FRAMEWORK_RUNTIME)
        frameworkRuntime: ZLinkFrameworkRuntime, // object relocation, host 종료와 lifecycle 관측 표면이다.
        @Inject(ZLINK_ROUTE_MESH_RUNTIME)
        routeMeshRuntime: ZLinkRouteMeshRuntime | null, // 동적 구성에 RouteMesh 역할이 없으면 null이다.
        @Inject(ZLINK_CLIENT_SERVER_RUNTIME)
        clientServerRuntime: ZLinkClientServerRuntime | null, // 동적 구성에 ClientServer 역할이 없으면 null이다.
        @Inject(ZLINK_FANOUT_RUNTIME)
        fanoutRuntime: ZLinkFanoutRuntime | null, // 동적 구성에 automatic subscriber가 없으면 null이다.
    ) {}
}
```

네 provider는 public runtime interface만 노출하며 내부 socket monitor나 private runtime object를 주입하지
않는다. Fanout runtime은 manual endpoint mutation handle을 제공하지 않는다.

> **`forRoot`와 `forRootFactory`의 실패 모양이 다르다.** 정적 `forRoot`에서 역할이 없으면
> **provider 자체가 등록되지 않는다.** `forRootFactory`처럼 동적으로 구성하는 경로에서는 역할이
> 없을 때 **provider 값이 `null`이 될 수 있다.** 주입 지점에서 두 경우를 구분해 다뤄야 한다.

**decorator의 책임 분리:**

- **channel handler**는 decorator로 group 이름을 붙이고, **channel이 그 group을 선택한다.**
- **Spot actor handler**는 decorator로 대상 Spot 타입을 명시한다.
- **Spot timer handler**도 decorator로 표시하고, module이 `zlinkDiscoverProviders(...)`로 수집한다.

**이렇게 나눠야 "channel이 어떤 handler 묶음을 받을지"와 "Spot·session이 자기 내부 메시지를 어떻게
처리할지"가 섞이지 않는다.**

## 5. Lifecycle

NestJS provider lifecycle hook에 runtime을 배선한다.

| hook | 시점 |
|---|---|
| `onModuleInit()` | **모든 provider가 DI에서 resolvable해진 뒤** runtime 시동(bind·connect·discovery) |
| `onModuleDestroy()` | application shutdown hook이 실행되지 않은 경우 `Shutdown`을 시작하고 같은 terminal result를 기다림 |
| `onApplicationShutdown()` | 진행 중인 host 종료에 합류하거나 `Shutdown`을 시작하고 runtime 자원을 정리 |

**`onModuleInit()`에서 시동하는 이유는 socket bind/connect와 discovery가 시작되려면 handler
provider가 모두 resolvable해야 하기 때문이다.**

### 5.1 시동 순서

lifecycle 참여자는 **framework → monitoring** 순서다.

1. backend channel adapter로 context를 생성한다.
2. MeshNode를 시작하고 RouteMesh ROUTER를 bind한다.
3. location runtime과 자동 연결을 준비한다.
4. channel receive loop와 stream node를 시작한다.
5. monitoring source를 준비된 runtime에 attach한다.

**시동은 idempotent해야 한다.** monitoring hook이 같은 runtime을 다시 시동시켜도 두 번 시작되지
않는다.

### 5.2 종료 순서

NestJS [shutdown](../../../01-glossary.ko.md#shutdown) hook은 host 단위 `Shutdown`을 사용한다. Rolling maintenance에서 continuity가 필요하면
operator가 hook 전에 주입받은 `ZLinkFrameworkRuntime.relocate(...)`를 호출하고 `Relocated` 결과를 확인한 뒤
`shutdown(...)`을 호출한다. Relocation이 필요하지 않으면 hook에서 `shutdown(...)`만 호출한다. Hook이
시작될 때 이미 `Shutdown`이 `Draining`을 시작했다면 새 operation을 만들지 않고 그 shared operation에 합류한다.

1. Framework runtime의 host maintenance barrier에서 신규 application admission을 닫는다.
2. 이미 수락한 작업과 진행 중인 relocation·STREAM barrier를 deadline까지 처리한다.
3. Spot·Actor authority, descriptor, listener와 raw transport를 Framework runtime이 정리한다.
4. terminal result와 event를 완료한 뒤 monitoring observer를 닫는다.
5. NestJS adapter가 registration과 backend context를 마지막에 정리한다.

### 5.3 fail-fast

**startup에서 runtime state를 만들다 한 컴포넌트라도 실패하면, 그때까지 만든 state를 그 자리에서
dispose한 뒤 예외를 다시 던진다.** 반쯤 열린 socket이나 매달린 context를 남기지 않는다.

내부 정리 순서는 [runtime-lifecycle](../../../../internals/README.ko.md)이,
backend 어댑터 포트는
[backend-dependency-policy](../../../../../node/internals/backend-dependency-policy.ko.md)가 소유한다.

## 6. RouteMesh 등록

`zlinkFramework()` fluent builder로 선언한다.

| 역할 | 의미 | bind |
|---|---|---|
| `addRouteMesh(...)` | 물리 MeshName과 MeshNode를 등록한다 | **필요** |
| `listen(port?)` | MeshNode가 공유하는 ROUTER listener를 연다. 자동 discovery에서 생략하면 port 0을 사용한다 | 불필요 |
| `channel(name).server()` | 논리 server membership과 handler namespace를 추가한다 | 불필요 |
| `channel(name).client()` | server [membership](../../../01-glossary.ko.md#membership) 없이 ChannelName 호출 역할을 추가한다 | 불필요 |
| `addClientServerChannel(name)` | 단방향 request 시작 권한이 구분된 별도 topology를 구성한다 | role에 따름 |
| `peerConnections()` | endpoint 또는 expected RID가 있는 manual peer intent를 추가한다 | 불필요 |
| `enablePublisher(...)` | 이 channel로 event를 publish한다 | **필요** |
| `enableSubscriber(...)` | 이 channel의 event를 받는다 | 불필요 |

자동·수동 연결, dispatch key와 중복 검사 범위는
[channel-topology §5](../../../07-channel-topology.ko.md)와
[channel-messaging](../../../08-channel-messaging.ko.md)이 소유한다.

## 7. Spot·Actor 등록

Spot·Actor factory는 owner MeshNode에 등록한다. [Spot direct](../../../01-glossary.ko.md#spot-direct)와 Logical Multicast는 Node·Channel
메시징과 같은 ROUTER를 사용한다. Discovery는 등록된 Redis location store를 사용한다.

| builder | 켜는 것 |
|---|---|
| `addRouteMesh(meshName).listen(port?)` | [owner](../../../01-glossary.ko.md#owner) MeshNode와 ROUTER listener |
| `channel(name).server()` | [Logical Multicast](../../../01-glossary.ko.md#logical-multicast) 범위와 [handler namespace](../../../01-glossary.ko.md#handler-namespace) |
| `channel(name).client()` | server membership이 없는 outbound [ChannelName](../../../01-glossary.ko.md#channelname) 호출 |
| `configureSpotPublisher()` | Logical Multicast의 ROUTER 송신 설정 |
| `addEntrySpot(TEntrySpot)` | Entry Spot handler registry 타입 |
| `addSpotFactory(TSpot)` | 이 노드가 만들 수 있는 spot 타입 |
| `addInstanceSpotFactory(type, TSpot, placement, relocation)` | 이 노드가 activation할 수 있는 actor-free [Instance Spot](../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) 타입 |
| MeshNode channel client | Spot handler의 ChannelName send/request가 공유하는 client |

중복 등록과 타입 규칙은
[MeshNode](../../../13-mesh-node.ko.md)와 [spot-messaging](../../../12-spot-messaging.ko.md)이 소유한다.

### 7.1 Entry Spot identity와 membership

Framework는 MeshNode startup에서 Entry Spot의 global Spot ID를 발급한다. 애플리케이션은 Entry Spot ID를
구성하거나 변경하지 않는다. Startup은 Entry Spot factory와 handler를 초기화하고 Ready barrier를 완료한 뒤
[descriptor](../../../01-glossary.ko.md#descriptor)를 게시한다. Actor create는 선택한 owner MeshNode의 Entry Spot membership과 Actor [Ready](../../../01-glossary.ko.md#ready) barrier를
같은 lifecycle에서 완료한다.

이 순서는 Framework runtime의 내부 책임이다. Public interface에는 transport 객체, local handle 또는 resolver를
노출하지 않는다.

Spot message의 실패와 Spot 수명 규칙은
[spot-messaging §6](../../../12-spot-messaging.ko.md#6-실패와-수명)이 소유한다. 수동 outbound
peer는 route mesh builder의 `connect(...)`로 지정한다.

### 7.2 Instance Spot 등록

Instance Spot factory는 배포 사이에도 유지되는 stable type, actor-free Spot provider, placement limit과
relocation policy를 함께 등록한다. 같은 MeshNode에서 같은 [stable type](../../../01-glossary.ko.md#stable-type)이나 같은 provider class를 User Spot
factory와 중복 등록하면 socket bind 전에 구성 오류로 실패한다.

Instance Spot provider는 direct packet과 timer handler만 등록할 수 있다. Actor handler나 Logical Multicast
subscription을 등록하면 location을 `Ready`로 바꾸기 전에 activation이 실패한다. Provider scope는 activation이
실패하거나 Instance Spot이 닫힐 때 한 번만 정리한다.

Spot direct fluent call의 Instance intent만 global [Spot ID](../../../01-glossary.ko.md#spot-id), stable type과 최초 Mesh를 durable creation intent로
기록한다. Stable type을 생략하면 선택한 Mesh의 serving descriptor에 distinct Instance type이 하나일 때 자동
선택하고 여러 type이면 caller가 stable type을 명시한다. Marker가 없는 일반 message는 Spot ID만 받으며
missing RID에 intent를 만들거나 factory를 시작하지 않는다. Application은 target node, owner token, generation
또는 retry option을 전달하지 않는다.

Ready location은 global Spot ID와 exact object generation을 포함하는 immutable `SpotRef`로 관측한다. 일반
message는 ref가 아니라 Spot ID를 사용하고, exact ref는 close에만 사용한다. Store version과 owner fence는
Framework 내부에 유지하며 application callback에 전달하지 않는다.

## 8. STREAM 등록

- **decorator 기반 암시 등록으로 열지 않는다.** `streams` 설정의 명시 등록만 기본 표면이다.
- **한 stream node에는 session을 하나만 둔다.**
- **bind endpoint는 반드시 있어야 한다.**
raw stream의 `write(...)`, `close(...)` 시그니처는
[Channel과 routing 인터페이스](interfaces/02-channel-messaging.ko.md)가 소유하고, backpressure 의미는
[stream-session](../../../19-stream-session.ko.md)이 소유한다.

## 9. Session actor dispatch 등록

계약은 [session-actor-dispatch](../../../20-session-actor-dispatch.ko.md)가 소유한다. Stream node에서 Actor dispatch를
활성화하면 runtime이 global Actor ID와 current [authority](../../../01-glossary.ko.md#authority)로 route를 결정한다. Application은 [MeshName](../../../01-glossary.ko.md#meshname)이나 Spot
resolver를 추가로 등록하지 않는다. Bound-session push는 current connection에만 적용되는 one-way operation이며
stale binding을 새 connection으로 retarget하지 않는다.

## 10. Monitoring · Location 등록

계약은 [runtime-monitoring](../../../24-runtime-monitoring.ko.md)과
[location-runtime](../../../21-location-runtime.ko.md)이 소유한다.

| 대상 | 등록 조건 |
|---|---|
| socket source | 이름이 `<channel>.<capability>` 형식이고 **그 channel 역할이 등록되어 있어야 한다** |
| location source | **polling 주기를 반드시 명시한다.** location runtime이 등록되어 있어야 한다 |
| mesh source | **등록된 MeshName**을 가리켜야 한다 |
| location store | **물리 저장소 인스턴스 하나**를 등록 루트에서 **한 번만** 둔다. 메모리 store와 함께 등록하면 설정 오류다 |

**임의 source 자동 발견은 지원하지 않는다.**

Redis store는 `@zlink-systems/framework-locations-redis`가 제공한다(§1).

## 11. Startup validation

검증 항목의 정본은
[channel-messaging §9](../../../08-channel-messaging.ko.md#9-검증-요구)와
[spot-messaging §8](../../../12-spot-messaging.ko.md)이 소유한다.

**Node는 모든 위반을 startup 시점 설정 예외로 던진다.** 설정 실수를 즉시 드러내는 쪽이 기본
규칙이다.

## 12. 회귀 테스트

등록과 startup validation의 회귀 항목은
[regression-test-matrix](../../../../../node/internals/regression-test-matrix.ko.md)가 소유한다.
