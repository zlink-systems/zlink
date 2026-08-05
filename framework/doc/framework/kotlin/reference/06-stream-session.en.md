# 06. Stream session

[Reference index](README.en.md)

Session bind failure rules, physical disconnect handling, and relocation route refresh are
exactly the same as
[Java reference 06. Stream session](../../java/reference/06-stream-session.ko.md) (Korean-only).
What Kotlin adds is a suspending session base class and Kotlin-only wrappers around one-way
calls. The exact signatures are owned by the
[Kotlin STREAM session exact interface](../../common/spec/server/languages/kotlin/interfaces/stream-session.en.md)
(Korean-only).

---

## `ZLinkSuspendingSession`

An abstract base class that overrides the session lifecycle as suspend functions.

```kotlin
class GameSession(private val ctx: ZLinkSessionContext) : ZLinkSuspendingSession() {
    override fun context(): ZLinkSessionContext = ctx

    override suspend fun onConnectedSuspending() { ... }
    override suspend fun onDisconnectedSuspending() { ... }
    override suspend fun onErrorSuspending(error: ZLinkStreamError) { ... }
}
```

**Options.** The overridable suspend methods are `onConnectedSuspending`,
`onDisconnectedSuspending`, `onErrorSuspending(error)`, and `onDispatchSuspending(dispatch,
payload)` (fallback dispatch). Typed packets are implemented separately as
`ZLinkSuspendingTypedSessionPacketHandler<TSessionContext, TMessage>` (`packetName()`,
`messageType()`, `suspend fun handle(context, dispatch, message)`) and registered via
`addSessionPacketHandler(...)` (topology-discovery category).

**Completion result.** `final override fun onConnected/onDisconnected/onError/onDispatch` only
act as a Java `CompletionStage` bridge. Completion ordering and handshake-failure handling are
the same as the Java reference's document 06.

**When to use.** Inherit this base class when implementing a Session with Kotlin coroutines.

---

## `send` / `reply` (ZLinkKotlinSessionClient)

Sends a one-way message to the connected client, or responds to the current request.

```kotlin
sessionContext.client().send(ServerTick(tickNumber)).await()
sessionContext.client().reply(GetPlayerStateResult(state)).await()
```

**Options.** `ZLinkKotlinSessionSendCall` provides `.metadata(...)`, `.compress()`, `.await()`,
and `ZLinkKotlinSessionReplyCall` provides `.compress()`, `.await()` (as with Java, `reply` has
no metadata modifier).

**Completion result.** Same completion kinds as the Java reference's `send`/`reply`. The
application only waits on local STREAM queue admission via `await(): Unit` and does not use
Java's `CompletionStage` and submission result type directly.

**When to use.** Same as the `send` (inside Session code)/`reply` entry in the Java reference.

---

## `bindOrGetActor` (ZLinkSessionActors extension)

Binds an Actor to this STREAM session. A suspend extension function wrapping Java's
`bindOrGet(actorRef)`.

```kotlin
val bound = sessionContext.actors().bindOrGetActor(actorRef)
```

**Options.** This function has no modifiers — it only takes `ActorRef`.

**Completion result.** Same completion kinds as the Java reference's `bind`/`bindOrGet`
(`NotFound`/`InvalidOperation`/`Unavailable`).

**When to use.** Same as the Java reference — bind when an Actor must push directly over this
client connection.

---

## `relay` (ZLinkKotlinSessionActor)

Delivers a payload from the Actor side to the client through the handle obtained from bind.

```kotlin
sessionActor.relay(ZLinkMessage.of(RoomUpdated(state))).await()
```

**Options.** Returns `ZLinkKotlinSubmissionCall` (only `.await()`). There is an overload taking
only the payload, and one that also takes a `ZLinkSessionDispatchContext` — the latter hands off
the explicit reply capability to the runtime (see the `relay` entry in the Java reference's
document 06). `notifyDisconnected()` uses Java's `ZLinkSessionActor.notifyDisconnected()`
directly with no Kotlin wrapper.

**Completion result.** Same as the Java reference.

**When to use.** Same as the `relay`/`notifyDisconnected` entry in the Java reference.

---

## `send` (ZLinkKotlinBoundSession, inside Actor code)

Sends a one-way message from an Actor to the client bound to it.

```kotlin
context.boundSession().send(InventoryChanged(item)).await()
```

**Options.** Returns `ZLinkKotlinMessageSendCall` (`.metadata(...)`, `.await()`). The
disconnecting `disconnect()` uses Java's `ZLinkBoundSession.disconnect()` directly with no
Kotlin wrapper.

**Completion result and when to use.** Same as the `send` (bound session)/`disconnect` entry in
the Java reference.

---

## `close` (ending a connection)

Calls Java's `ZLinkSessionContext.close()` directly (chain with `.await()` if needed). The
completion rules are the same as the
[`close` entry in the Java reference's document 06](../../java/reference/06-stream-session.ko.md)
(Korean-only).

---

See the
[Kotlin STREAM session exact interface](../../common/spec/server/languages/kotlin/interfaces/stream-session.en.md)
and
[Java reference 06. Stream session](../../java/reference/06-stream-session.ko.md) (Korean-only)
for the full rationale.
