# 02. Topology discovery

[레퍼런스 목차](README.ko.md)

Topology 등록(`addRouteMesh`/`addClientServerChannel`/`addFanoutChannel`/`addStreamNode`,
Object role·factory 등록, Manual peer 연결, `useFilter`, 기타 host-wide 옵션, 런타임 weight
조회·변경, Topology 상태 조회·관찰)은 Java builder를 그대로 호출한다 — 정확한 signature와 옵션
표는 [Java 레퍼런스 02. Topology discovery](../../java/reference/02-topology-discovery.ko.md)를
그대로 따른다. Kotlin이 추가하는 것은 receiver lambda DSL과 handler dispatcher 선택뿐이다.
정확한 signature는
[Kotlin 구성과 host exact interface](../../common/spec/server/languages/kotlin/interfaces/configuration-host.ko.md)와
[Kotlin channel messaging exact interface](../../common/spec/server/languages/kotlin/interfaces/channel-messaging.ko.md)가
소유한다.

---

## `routeMesh { }` / `channel { }` (구성 시점, DSL)

`addRouteMesh(meshName)`과 `mesh.channel(channelName)`을 receiver lambda로 감싼 DSL이다. Java
builder의 의미를 바꾸지 않는다.

```kotlin
options.routeMesh("play") {
    listen(5501)
    setRoutingIdPrefix("play")
    setPlacementWeight(100)

    channel("play.api") {
        server()
            .setWeight(100)
            .addHandlerGroup("api")
    }
}
```

**옵션.** `configure: ZLinkMeshNodeBuilder.() -> Unit`(routeMesh)과
`configure: ZLinkMeshChannelBuilder.() -> Unit = {}`(channel, 기본값은 빈 람다)를 받는다. 그 안에서
호출하는 개별 modifier는 Java 레퍼런스 02번 문서의 `addRouteMesh`/RouteMesh Channel 등록 항목과
완전히 같다.

**완료 결과.** Java builder를 그대로 반환한다 — 새 의미를 추가하지 않는다.

**선택 기준.** Kotlin 코드에서 중첩 구성을 receiver 스타일로 쓰고 싶을 때 이 DSL을 쓴다. Java
builder를 직접 호출해도 동작은 같다.

---

## `useCoroutineHandlers(...)` (구성 시점)

Handler dispatch에 쓸 `CoroutineDispatcher`(선택적으로 `CoroutineScope`)를 지정한다.

```kotlin
options.useCoroutineHandlers(Dispatchers.Default)

// 커스텀 scope를 함께 지정하는 경우
options.useCoroutineHandlers(applicationScope, Dispatchers.Default)
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `useCoroutineHandlers(dispatcher)` | Java 레퍼런스 02번의 `useVirtualThreadHandlers()`/`useHandlerExecutor(...)`와 상호 배타 | Suspending handler를 실행할 `CoroutineDispatcher` |
| `useCoroutineHandlers(scope, dispatcher)` | — | Handler coroutine이 속할 `CoroutineScope`까지 함께 지정 |

**완료 결과.** 반환값 없이 동기로 등록된다.

**선택 기준.** Suspending handler(`ZLinkSuspendingRequestHandler` 등, messaging-execution·spot-instance·
actor-relocation category 참고)를 쓰는 host가 dispatcher를 명시할 때 쓴다. Java 레퍼런스의
`useVirtualThreadHandlers()`/`useHandlerExecutor(...)`와는 상호 배타적으로 선택한다.

---

## `actorFactory { }` (구성 시점, reified DSL)

`ZLinkMeshObjectServerBuilder.addActorFactory(...)`를 reified type parameter로 감싼 DSL이다.

```kotlin
serverBuilder.actorFactory<PlayerActor, PlayerActorFactory>("player") {
    preserveStateWith(PlayerRelocationAdapter::class.java)
}
```

**옵션.** `configure: ZLinkActorFactoryBuilder<TActor>.() -> Unit`를 받는다. Relocation 정책
선택(`disableRelocation()`/`recreateOnRelocation()`/`preserveStateWith(...)`)은
actor-relocation category와 같다 — Actor factory builder에는 relocation 동작 선택 외의 설정이
없다.

**완료 결과.** Java 레퍼런스의 Object role 등록 항목과 같다. Callback을 생략하거나 정책을 둘 이상
호출하면 startup configuration error다.

**선택 기준.** `TActor`/`TFactory` 타입 인자를 `Class<T>`로 직접 넘기지 않고 reified generic으로
쓰고 싶을 때 이 DSL을 쓴다.

---

## `configureDispatch { }` / `configureStreamCompression { }` (구성 시점, DSL)

각각 `ZLinkDispatchOptions`/`ZLinkStreamCompressionBuilder`를 receiver로 받는 DSL이다.

```kotlin
options.configureDispatch {
    messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
}

options.configureStreamCompression {
    useLz4()
}
```

**옵션.** `configureDispatch`는 `ZLinkDispatchOptions`를 반환하고, `configureStreamCompression`은
`ZLinkFrameworkOptions`를 반환한다(Java builder의 반환 형태 차이를 그대로 유지). 안에서 호출하는
개별 modifier는 observability-diagnostics category(diagnostics)와 Java 레퍼런스 02번의 "기타
host-wide 옵션"(stream compression) 항목과 같다.

**완료 결과.** Java builder를 그대로 반환한다.

**선택 기준.** Kotlin 코드에서 receiver 스타일로 diagnostics·압축 설정을 묶고 싶을 때 쓴다.

---

## `ZLinkMeshPeerConnections.connect(expectedRoutingId, endpoint)` (구성 시점·런타임)

Java `peerConnections().connect(expectedRoutingId, endpoint)`와 같은 동작을 확장 함수 형태로
제공한다. Manual peer 연결의 완료 규칙은 Java 레퍼런스 02번의 "Manual peer 연결" 항목과 완전히
같다 — 이 문서에서 반복하지 않는다.

---

전체 근거는
[Kotlin 구성과 host exact interface](../../common/spec/server/languages/kotlin/interfaces/configuration-host.ko.md),
[Kotlin channel messaging exact interface](../../common/spec/server/languages/kotlin/interfaces/channel-messaging.ko.md)와
[Java 레퍼런스 02. Topology discovery](../../java/reference/02-topology-discovery.ko.md)를 참고한다.
