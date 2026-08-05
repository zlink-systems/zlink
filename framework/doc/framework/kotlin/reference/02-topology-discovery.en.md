# 02. Topology discovery

[Reference index](README.en.md)

Topology registration (`addRouteMesh`/`addClientServerChannel`/`addFanoutChannel`/
`addStreamNode`, Object role/factory registration, Manual peer connections, `useFilter`, other
host-wide options, runtime weight query/change, topology status query/observation) calls the Java
builders directly as-is — the exact signature and options table follow
[Java reference 02. Topology discovery](../../java/reference/02-topology-discovery.ko.md)
(Korean-only) directly. The only things Kotlin adds are the receiver-lambda DSL and handler
dispatcher selection. The exact signatures are owned by the
[Kotlin configuration and host exact interface](../../common/spec/server/languages/kotlin/interfaces/configuration-host.en.md)
and the
[Kotlin channel messaging exact interface](../../common/spec/server/languages/kotlin/interfaces/channel-messaging.en.md)
(Korean-only).

---

## `routeMesh { }` / `channel { }` (configuration time, DSL)

A DSL that wraps `addRouteMesh(meshName)` and `mesh.channel(channelName)` in a receiver lambda.
It does not change the Java builder's meaning.

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

**Options.** Takes `configure: ZLinkMeshNodeBuilder.() -> Unit` (routeMesh) and
`configure: ZLinkMeshChannelBuilder.() -> Unit = {}` (channel, defaulting to an empty lambda). The
individual modifiers called inside are exactly the same as the `addRouteMesh`/RouteMesh Channel
registration entries in the Java reference's document 02.

**Completion result.** Returns the Java builder as-is — it adds no new meaning.

**When to use.** Use this DSL when you want nested configuration in Kotlin code in receiver
style. Calling the Java builder directly behaves the same.

---

## `useCoroutineHandlers(...)` (configuration time)

Specifies the `CoroutineDispatcher` (optionally a `CoroutineScope`) to use for handler dispatch.

```kotlin
options.useCoroutineHandlers(Dispatchers.Default)

// specifying a custom scope as well
options.useCoroutineHandlers(applicationScope, Dispatchers.Default)
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `useCoroutineHandlers(dispatcher)` | Mutually exclusive with the Java reference document 02's `useVirtualThreadHandlers()`/`useHandlerExecutor(...)` | The `CoroutineDispatcher` to run suspending handlers on |
| `useCoroutineHandlers(scope, dispatcher)` | — | Also specifies the `CoroutineScope` the handler coroutines belong to |

**Completion result.** Registers synchronously with no return value.

**When to use.** Use this when a host that uses suspending handlers (`ZLinkSuspendingRequestHandler`
and others — see the messaging-execution/spot-instance/actor-relocation categories) must specify
a dispatcher. Choose this mutually exclusively with the Java reference's
`useVirtualThreadHandlers()`/`useHandlerExecutor(...)`.

---

## `actorFactory { }` (configuration time, reified DSL)

A DSL that wraps `ZLinkMeshObjectServerBuilder.addActorFactory(...)` with a reified type
parameter.

```kotlin
serverBuilder.actorFactory<PlayerActor, PlayerActorFactory>("player") {
    preserveStateWith(PlayerRelocationAdapter::class.java)
}
```

**Options.** Takes `configure: ZLinkActorFactoryBuilder<TActor>.() -> Unit`. Relocation policy
selection (`disableRelocation()`/`recreateOnRelocation()`/`preserveStateWith(...)`) is the same as
the actor-relocation category — the Actor factory builder has no configuration beyond the
relocation behavior choice.

**Completion result.** Same as the Object role registration entry in the Java reference. Omitting
the callback, or calling more than one policy, is a startup configuration error.

**When to use.** Use this DSL when you want to pass the `TActor`/`TFactory` type arguments as a
reified generic rather than as `Class<T>` directly.

---

## `configureDispatch { }` / `configureStreamCompression { }` (configuration time, DSL)

DSLs that each take `ZLinkDispatchOptions`/`ZLinkStreamCompressionBuilder` as the receiver.

```kotlin
options.configureDispatch {
    messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
}

options.configureStreamCompression {
    useLz4()
}
```

**Options.** `configureDispatch` returns `ZLinkDispatchOptions`, and `configureStreamCompression`
returns `ZLinkFrameworkOptions` (preserving the Java builders' differing return shapes as-is).
The individual modifiers called inside are the same as the observability-diagnostics category
(diagnostics) and the "Other host-wide options" entry (stream compression) in the Java
reference's document 02.

**Completion result.** Returns the Java builder as-is.

**When to use.** Use this when you want to group diagnostics/compression settings in receiver
style in Kotlin code.

---

## `ZLinkMeshPeerConnections.connect(expectedRoutingId, endpoint)` (configuration time and runtime)

Provides the same behavior as Java's `peerConnections().connect(expectedRoutingId, endpoint)` in
the form of an extension function. The completion rules for Manual peer connections are exactly
the same as the "Manual peer connections" entry in the Java reference's document 02 — not
repeated in this document.

---

See the
[Kotlin configuration and host exact interface](../../common/spec/server/languages/kotlin/interfaces/configuration-host.en.md),
[Kotlin channel messaging exact interface](../../common/spec/server/languages/kotlin/interfaces/channel-messaging.en.md),
and
[Java reference 02. Topology discovery](../../java/reference/02-topology-discovery.ko.md)
(Korean-only) for the full rationale.
