# 06. Stream session

[Reference index](README.en.md)

This category covers the entry points used inside STREAM session code (`ZLinkSession`,
`ZLinkSessionClient`, `ZLinkSessionActors`, `ZLinkSessionActor`) and the entry point used for a
bound session inside Actor code (`ZLinkBoundSession`). The exact signatures are owned by the
[Java STREAM session exact interface](../../common/spec/server/languages/java/interfaces/stream-session.en.md)
and the
[Java Actor exact interface](../../common/spec/server/languages/java/interfaces/actors.en.md)
(Korean-only).

---

## Session callback implementation (`ZLinkSession`)

Processes the lifecycle events and typed packets this STREAM session receives. Implemented by the
type registered via `registerSession(...)` (topology-discovery category).

```java
public class GameSession implements ZLinkSession {
    @Override
    public ZLinkSessionContext context() { return context; }

    @Override
    public CompletionStage<Void> onConnected() { ... }

    @Override
    public CompletionStage<Void> onDisconnected() { ... }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) { ... }
}
```

**Options.** Typed packets are not placed on the session class itself — a separate handler type
is implemented as `ZLinkTypedSessionPacketHandler<TSessionContext, TMessage>` and registered via
`addSessionPacketHandler(...)` (topology-discovery category).

| Handler interface | Registration |
| --- | --- |
| `ZLinkTypedSessionPacketHandler<TSessionContext, TMessage>` | Declares the type it processes via `messageType()`, and implements `handle(context, dispatchContext, message)`. Registered via `addSessionPacketHandler(handlerType)` |
| `ZLinkSession.onDispatch(dispatchContext, message)` (default method) | A fallback for a packet the typed handler above did not process. The override is optional |

**Completion result.** All callbacks return `CompletionStage<Void>`. `onConnected`/
`onDisconnected` each run once per connect/disconnect, `onError` on every transport error, and
the typed handler once per packet after the Framework's internal recv loop finishes header
framing and queue admission. A handshake failure happens before the session is created, so it is
recorded only in runtime monitoring, not `onError`.

**When to use.** Every host that uses the `stream-session` topology implements this. Only a
packet whose `ZLinkSessionDispatchContext.canReply()` is `true` can be answered with `reply`.

---

## `send` (ZLinkSessionClient)

Sends a one-way message to the connected client. Called via the `ZLinkSessionClient` that
`ZLinkSessionContext.client()` returns.

```java
sessionContext.client().send(new ServerTick(tickNumber)).submit();
```

**Options.** `ZLinkSessionSendCall` provides the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` | None | Key-value to pass to the client |
| `.compress()` | Uncompressed | Compresses the payload with the registered stream compression codec |
| `.submit()` | Required terminal | Waits only until source-local admission |

**Completion result.** The same one-way completion kinds as the messaging-execution category —
waits until the socket send timeout and, if still not admitted, completes as a
`ZLinkFrameworkException` with `DEADLINE_EXCEEDED`; a connection disconnect is `UNAVAILABLE`.

**When to use.** Use this for a server-initiated push message, not a client-sent request. Use
`reply` to answer a client's request.

---

## `reply` (ZLinkSessionClient)

Responds to the request packet currently being processed.

```java
sessionContext.client().reply(new GetPlayerStateResult(state)).submit();
```

**Options.** `ZLinkSessionReplyCall` provides the following modifiers — `.compress()` and the
required terminal `.submit()`. Unlike `send`, there is no metadata modifier.

**Completion result.** Atomically claims this request's one-shot reply token and then sends. A
second `reply` call made with the same token fails to claim it, does not attempt the transport,
and ends as an exceptional completion. The caller's request timeout is not carried over the wire,
so this reply's admission deadline uses only the STREAM socket send timeout.

**When to use.** Use this only for a packet (request) whose
`ZLinkSessionDispatchContext.canReply()` is `true`. Use `send` to send a new message that was not
client-initiated.

---

## `bind` / `bindOrGet` (ZLinkSessionActors)

Binds an Actor to this STREAM session so the Actor side can push over this connection. Called via
`ZLinkSessionContext.actors()`.

```java
ZLinkSessionActor bound = sessionContext.actors().bindOrGet(actorRef)
    .toCompletableFuture().get();
```

**Options.** This call has no modifiers — it only takes `ActorRef`.

**Completion result.** `bind` creates a new binding every time. `bindOrGet` returns the
already-bound one if the same incarnation is already bound. A binding is fixed to one exact
incarnation of `actorId + objectGeneration`. No active Message Follow route completes with
`NOT_FOUND`, a differing generation with `INVALID_OPERATION`, and a pre-commit seal in progress
with `UNAVAILABLE`. `find(actorId)` synchronously queries an already-bound handle, and `bound()`
returns the full list bound to the current session.

**When to use.** Bind when an Actor must push directly over this client connection. Even across
a relocation, `ZLinkSessionActor.ref()` refreshes to the current location snapshot, so the
application does not need to bind again.

---

## `relay` / `notifyDisconnected` (ZLinkSessionActor)

Delivers a payload from the Actor side to the client, or notifies of a connection disconnect,
through the `ZLinkSessionActor` obtained from bind.

```java
sessionActor.relay(ZLinkMessage.of(new RoomUpdated(state)))
    .toCompletableFuture().get();
```

**Options.** Neither call has modifiers — both only take the payload (`relay`). `relay` also has
an overload that takes a `ZLinkSessionDispatchContext`.

**Completion result.** `relay` taking only the payload is a one-way operation that completes
normally once source-local admission is accepted. The overload taking a dispatch context
immediately hands off the explicit current STREAM request reply capability to the runtime — once
submitted, a typed reply completes the original correlation terminal-once, and an admission
failure completes the same correlation as a typed failure. `notifyDisconnected` is a notification
that signals a logical disconnect while the connection remains open, and waits until the
callback's terminal. Because a physical disconnect is automatically notified by the Framework to
every current binding, this call is not a substitute path for that.

**When to use.** Use this from Actor-side code to deliver directly to a specific bound client. A
reply to a request is handled by `reply` on the Session side.

---

## `send` (ZLinkBoundSession, inside Actor code)

Sends a one-way message from an Actor to the client bound to it. Called via the
`ZLinkBoundSession` that `ZLinkActorContext.boundSession()` returns.

```java
context.boundSession().send(new InventoryChanged(item)).submit();
```

**Options.** `ZLinkBoundSessionSendCall` provides `.metadata(...)` and the required terminal
`.submit()`.

**Completion result.** The same one-way completion kinds as the messaging-execution category.
This surface does not provide a new request operation aimed at the client — a reply to a client
request is handled by the Actor request handler's return value.

**When to use.** Use this from Actor-side code to push to the bound client. Use the `send`
(ZLinkSessionClient) entry above to send directly from the Session side. Use
`ZLinkBoundSession.disconnect()` to disconnect.

---

## `close` (ending a connection)

Closes the session. Provided by `ZLinkSessionContext.close()`.

```java
sessionContext.close().toCompletableFuture().get();
```

**Options.** This call has no modifiers.

**Completion result.** Closes the connection. It observes the remote unbind completion within a
bounded lifecycle deadline, and a timeout or terminal failure is returned as a close failure —
either way, the local binding and session transport are cleaned up regardless of success or
failure.

**When to use.** Use this when the application must voluntarily disconnect this STREAM
connection. Use `ZLinkBoundSession.disconnect()` to disconnect a bound client connection from the
Actor side.

---

## `disconnect` (inside Actor code, bound session)

Disconnects the client bound to an Actor. Called via `ZLinkBoundSession.disconnect()`.

```java
context.boundSession().disconnect().toCompletableFuture().get();
```

**Options.** This call has no modifiers.

**Completion result.** Disconnects the connection with the bound session.

**When to use.** Use this from Actor-side code when a specific client connection no longer needs
to be kept. Use the `close` entry to disconnect directly from the Session side.

---

See the
[Java STREAM session exact interface](../../common/spec/server/languages/java/interfaces/stream-session.en.md)
and the
[Java Actor exact interface](../../common/spec/server/languages/java/interfaces/actors.en.md)
(Korean-only) for the full rationale.
