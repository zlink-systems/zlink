# 06. Stream session

[Reference index](README.en.md)

This category covers the entry points used inside STREAM session code (`ZLinkSession`,
`ZLinkStream`, `ZLinkSessionActors`, `ZLinkSessionActor`) and the entry point used for a bound
session inside Actor code (`ZLinkBoundSession`). The exact signatures are owned by the
[Channel, request, and routing exact interface](../../common/spec/server/languages/node/interfaces/02-channel-messaging.en.md)
and the
[Foundation types and configuration exact interface](../../common/spec/server/languages/node/interfaces/01-foundation-configuration.en.md)
(Korean-only).

---

## Session callback implementation (`ZLinkSession`)

Processes the lifecycle/packet events this STREAM session receives. Implemented by the type
registered via `registerSession(...)` (topology-discovery category).

```ts
export class GameSession implements ZLinkSession {
  readonly context!: ZLinkSessionContext;

  async onConnected(context: ZLinkSessionContext) { ... }
  async onDisconnected(context: ZLinkSessionContext) { ... }
  async onError(context: ZLinkSessionContext, error: ZLinkStreamError) { ... }
}
```

**Options.** `onConnected`/`onDisconnected`/`onError`/`onDispatch` are all optional methods.
Typed packets are not placed on the session class itself — a separate handler class is
implemented as `ZLinkSessionPacketHandler<TSessionContext, TMessage>` and registered via
`@ZLinkStreamPacket()` (or the raw `ZLinkSessionHandlerRegistry.addHandler(handlerType)`).

| Handler | Registration |
| --- | --- |
| `ZLinkSessionPacketHandler<TSessionContext, TMessage>` | A typed handler. Implement `handle(context, dispatch, message)`, then register via `context.handlers.addHandler(handlerType)` |
| `ZLinkSession.onDispatch?(dispatch, payload)` | A fallback for a packet the typed handler above did not process |

**Completion result.** All callbacks return `Promise<void>`. `onConnected`/`onDisconnected` each
run once per connect/disconnect, `onError` on every transport error, and the typed handler once
per packet after the Framework's internal recv loop finishes header framing and queue admission.
A handshake failure happens before the session is created, so it is recorded only in runtime
monitoring, not `onError`.

**When to use.** Every host that uses the `stream-session` topology implements this. Only a
packet whose `ZLinkSessionDispatchContext.canReply` is `true` can be answered with `reply`.

---

## `write` (ZLinkStream, raw transport handle)

Writes a payload directly to the STREAM transport, bypassing a typed call.

```ts
const written = stream.write(ZLinkMessage.from(rawFrame));
```

**Options.** This call only has `flags?: number`.

**Completion result.** Returns a synchronous `boolean` — reports only whether admission
succeeded, and does not use the same Promise-based completion kinds as a typed call.

**When to use.** Use this only when low-level transport is needed that a typed `send`/`reply`
call cannot handle. Use `send` or `reply` for ordinary business messaging.

---

## `send` (ZLinkSessionClient)

Sends a one-way message to the connected client. Provided by `ZLinkSessionContext.client`.

```ts
await sessionContext.client.send(new ServerTick(tickNumber)).submit();
```

**Options.** `ZLinkSessionSendCall` provides the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` | None | Key-value to pass to the client |
| `.compress(enabled?)` | Uncompressed | Compresses the payload with the registered stream compression codec |
| `.submit(signal?)` | Required terminal | Waits only until source-local admission |

**Completion result.** The same one-way completion kinds as the messaging-execution category —
waits until the socket send timeout and, if still not admitted, completes with
`DeadlineExceeded`; a connection disconnect is `Unavailable`.

**When to use.** Use this for a server-initiated push message, not a client-sent request. Use
`reply` to answer a client's request.

---

## `reply` (ZLinkSessionClient)

Responds to the request packet currently being processed.

```ts
await sessionContext.client.reply(new GetPlayerStateResult(state)).submit();
```

**Options.** `ZLinkSessionReplyCall` only provides `.compress(enabled?)` and the required
terminal `.submit(signal?)` — unlike `send`, there is no metadata modifier.

**Completion result.** Atomically claims this request's one-shot reply token and then sends. A
second `reply` call made with the same token fails to claim it, does not attempt the transport,
and ends as an exceptional completion. The caller's request timeout is not carried over the wire,
so this reply's admission deadline uses only the STREAM socket send timeout.

**When to use.** Use this only for a packet (request) whose
`ZLinkSessionDispatchContext.canReply` is `true`. Use `send` to send a new message that was not
client-initiated.

---

## `bind` / `bindOrGet` (ZLinkSessionActors)

Binds an Actor to this STREAM session so the Actor side can push over this connection. Called via
`ZLinkSessionContext.actors`.

```ts
const bound = await sessionContext.actors.bindOrGet(actorRef);
```

**Options.** This call has no modifiers — it only takes `ActorRef` and an optional `signal`.

**Completion result.** `bind` creates a new binding every time. `bindOrGet` returns the
already-bound one if the same incarnation is already bound. A binding is fixed to one exact
incarnation of `actorId + objectGeneration`. No active Message Follow route completes with
`NotFound`, a differing generation with `InvalidOperation`, and a pre-commit seal in progress
with `Unavailable`. `find(actorId)` synchronously queries an already-bound handle, and `bound`
returns the full list bound to the current session.

**When to use.** Bind when an Actor must push directly over this client connection. Even across
a relocation, `ZLinkSessionActor.ref` refreshes to the current location snapshot, so the
application does not need to bind again.

---

## `relay` / `notifyDisconnected` (ZLinkSessionActor)

Delivers a payload from the Actor side to the client, or notifies of a connection disconnect,
through the `ZLinkSessionActor` obtained from bind.

```ts
await sessionActor.relay(ZLinkMessage.from(new RoomUpdated(state)));
```

**Options.** Neither call has modifiers — both only take the payload (`relay`) and an optional
`signal`. `relay` also has an overload that takes a `ZLinkSessionDispatchContext`.

**Completion result.** `relay` taking only the payload is a one-way admission that completes
normally once the local relay queue accepts the operation. The overload taking a dispatch
context immediately hands off the explicit current STREAM request reply capability to the
runtime — if admission succeeds, the Actor's typed reply completes the original STREAM
correlation terminal-once, and an admission failure completes the same correlation as a typed
failure. `notifyDisconnected` is a notification that signals a logical disconnect while the
connection remains open, and waits until the callback's terminal. Because a physical disconnect
is automatically notified by the Framework to every current binding, this call is not a
substitute path for that.

**When to use.** Use this from Actor-side code to deliver directly to a specific bound client. A
reply to a request is handled by `reply` on the Session side.

---

## `send` (ZLinkBoundSession, inside Actor code)

Sends a one-way message from an Actor to the client bound to it. Provided by
`ZLinkActorContext.boundSession`.

```ts
await context.boundSession.send(new InventoryChanged(item)).submit();
```

**Options.** `ZLinkBoundSessionSendCall` provides `.metadata(...)` and the required terminal
`.submit(signal?)`.

**Completion result.** The same one-way completion kinds as the messaging-execution category.
This surface does not provide a new request operation aimed at the client — a reply to a client
request is handled by the Actor request handler's return value.

**When to use.** Use this from Actor-side code to push to the bound client. Use the `send`
(ZLinkSessionClient) entry above to send directly from the Session side. Use
`ZLinkBoundSession.disconnect(signal?)` to disconnect.

---

## `close` (ending a connection)

Closes the session or the raw transport handle. Provided by
`ZLinkSessionContext.close(signal?)` and `ZLinkStream.close(signal?)` respectively.

```ts
await sessionContext.close(); // closes this connection from the Session side
await stream.close();         // closes directly from the transport handle
```

**Options.** Neither call has modifiers.

**Completion result.** Closes the connection. Calling it again on an already-closed connection
carries no separate exception contract this document defines — check the exact interface for the
precise re-call semantics.

**When to use.** Use this when the application must voluntarily disconnect this STREAM
connection. Use `ZLinkBoundSession.disconnect(signal?)` to disconnect a bound client connection
from the Actor side.

---

## `disconnect` (inside Actor code, bound session)

Disconnects the client bound to an Actor. Called via `ZLinkBoundSession.disconnect(signal?)`.

```ts
await context.boundSession.disconnect();
```

**Options.** This call only has an optional `signal`.

**Completion result.** Disconnects the connection with the bound session.

**When to use.** Use this from Actor-side code when a specific client connection no longer needs
to be kept. Use the `close` entry to disconnect directly from the Session side.

---

See the
[Channel, request, and routing exact interface](../../common/spec/server/languages/node/interfaces/02-channel-messaging.en.md)
and the
[Foundation types and configuration exact interface](../../common/spec/server/languages/node/interfaces/01-foundation-configuration.en.md)
(Korean-only) for the full rationale.
