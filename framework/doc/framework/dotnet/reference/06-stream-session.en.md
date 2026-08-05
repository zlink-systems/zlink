# 06. Stream session

[Reference index](README.en.md)

This category covers the entry points used inside STREAM session code
(`IZLinkSessionClient`, `IZLinkSessionActors`, `IZLinkSessionActor`, `IZLinkStream`) and the
entry points used inside Actor code for a bound session (`IZLinkBoundSession`). The exact
signatures are owned by the
[STREAM session exact interface](../../common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md)
and the
[Bound STREAM session exact interface](../../common/spec/server/languages/dotnet/interfaces/07-bound-stream-session.ko.md)
(both Korean-only).

---

## Handler registration (inside Session code, `Configure()`)

Registers the packet handler types this STREAM session receives. Call it through
`Context.Handlers` (`IZLinkSessionHandlerRegistry`), only inside the `Configure()` override.

```csharp
public void Configure()
{
    Context.Handlers.AddHandler<PingHandler>();
}
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.AddHandler<THandler>()` | packet name is derived from the message type | registers an `IZLinkSessionPacketHandler<TSessionContext, TMessage>` implementation |
| `.AddHandler<THandler>(packetName)` | — | explicitly specifies the packet name |

**Completion.** Registers synchronously with no return value. `TryHandleAsync(dispatch, payload,
ct)` is the internal entry point that attempts actual dispatch through the registered handler,
and application code does not call it directly — the Framework's recv loop calls it for every
packet it receives.

**When to use it.** Register every packet handler this session processes each time
`Configure()` is called.

---

## `Send<TMessage>` (inside Session code)

Sends a one-way message to the connected client. Call it through `Context.Client.Send(...)`.

```csharp
await Context.Client
    .Send(new ServerTick(tickNumber))
    .Async(ct);
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Metadata(...)` | none | key-value passed to the client |
| `.Compress()` | uncompressed | compresses the payload with the registered stream compression codec |
| `.Async(ct)` | required terminal | waits only until source-local admission |

**Completion.** Uses the same one-way completion kinds as the messaging-execution category —
waits until the socket send timeout, then `DeadlineExceeded` if still unsent; a broken
connection is `Unavailable`.

**When to use it.** Use it for a server-initiated push message, not a reply to a client's
request. To answer a client's request, use `Reply`.

---

## `Reply<TMessage>` (inside Session code)

Responds to the request packet currently being processed. Call it through
`Context.Client.Reply(...)`.

```csharp
await Context.Client
    .Reply(new GetPlayerStateResult(state))
    .Async(ct);
```

**Options.** This call has only `.Compress()` and the `.Async(ct)` terminal — `Reply` has no
metadata modifier.

**Completion.** It atomically claims this request's one-shot reply token before sending. A
second `Reply` call built from the same token fails to claim it, does not attempt transport, and
ends as an exceptional completion. Because the caller's request timeout is not carried over the
wire, this reply's admission deadline uses only the STREAM socket send timeout. It does not send
a late reply after timeout or cancellation.

**When to use it.** Use it only for a packet (request) where `ZLinkSessionDispatchContext.CanReply`
is true. To send a new message that is not a client's own request, use `Send`.

---

## `BindAsync` / `BindOrGetAsync` (Session↔Actor)

Binds an Actor to this STREAM session so the Actor side can push over this connection. Call it
through `Context.Actors`.

```csharp
IZLinkSessionActor bound = await Context.Actors.BindOrGetAsync(actorRef, ct);
```

**Options.** This call has no modifiers — it takes only an `ActorRef` and a `CancellationToken`.

**Completion.** `BindAsync` always creates a new binding. `BindOrGetAsync` returns the existing
one if the same incarnation is already bound. A binding is fixed to one exact incarnation of
`ActorRef.ActorId + ObjectGeneration`. It is `NotFound` if there is no mapping, `InvalidOperation`
if the generation differs, and `Unavailable` while a pre-commit seal is in progress.
`Find(actorId)` can synchronously look up an already-bound handle.

**When to use it.** Bind when an Actor needs to push directly over this client connection. Even
if relocation occurs, `IZLinkSessionActor.Ref` updates to the current location snapshot, so the
application does not need to bind again.

---

## `RelayAsync` / `NotifyDisconnectedAsync` (bound Actor handle)

Uses the `IZLinkSessionActor` obtained from binding to deliver a payload to the client from the
Actor side, or to notify it of a disconnect.

```csharp
await bound.RelayAsync(ZLinkMessage.From(new RoomUpdated(state)), ct);
```

**Options.** Neither call has modifiers — they take only the payload (`RelayAsync`) and a
`CancellationToken`.

**Completion.** `RelayAsync` is a one-way operation that completes normally once source-local
admission is accepted. `NotifyDisconnectedAsync` is a notification of a logical disconnect while
the connection is still maintained, and it waits until the callback terminates. A physical
disconnect is automatically notified by the Framework to the entire current binding, so this
call is not a substitute path for that.

**When to use it.** Use it from Actor-side code to deliver directly to a specific bound client.
A reply to a request is handled by the Session side's `Reply`.

---

## `Send<TMessage>` (inside Actor code, bound session)

Sends a one-way message from an Actor to the client bound to it. Call it through
`Context.BoundSession.Send(...)`.

```csharp
await Context.BoundSession
    .Send(new InventoryChanged(item))
    .Async(ct);
```

**Options.** Has `.Metadata(...)` and the required terminal `.Async(ct)`.

**Completion.** Uses the same one-way completion kinds as the messaging-execution category.
This surface does not provide a new request operation toward the client — a reply to a
client's request is handled by the Actor request handler's return value.

**When to use it.** Use it from Actor-side code to push to the bound client. To send directly
from the Session side, use the `Send` entry above (inside Session code). To disconnect, use
`Context.BoundSession.DisconnectAsync(ct)`.

---

## `Write` (raw transport handle)

Writes a payload directly to the STREAM transport, bypassing typed calls. Call it on the
`IZLinkStream` handle in a Session callback.

```csharp
bool written = stream.Write(ZLinkMessage.From(rawFrame), SendFlags.None);
```

**Options.** The following modifier attaches to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `flags: SendFlags` | `SendFlags.None` | low-level transport flags |

**Completion.** Returns a synchronous `bool` — it reports only whether admission succeeded and
does not use the exception-based completion kinds typed calls use.

**When to use it.** Use it only when a low-level transport need exceeds what typed `Send`/`Reply`
calls can handle. For ordinary business messaging, use `Send` or `Reply`.

---

## `CloseAsync` (closing the connection)

Closes the session or the raw transport handle. `IZLinkSessionContext.CloseAsync()` and
`IZLinkStream.CloseAsync()` each provide it.

```csharp
await Context.CloseAsync();  // closes this connection from the Session side
await stream.CloseAsync();   // closes it directly from the transport handle
```

**Options.** Neither call has modifiers.

**Completion.** Closes the connection. This document defines no separate exception contract for
calling it again on an already-closed connection — check the exact interface for the precise
re-call semantics.

**When to use it.** Use it when the application needs to voluntarily end this STREAM
connection. To disconnect a bound client connection from the Actor side, use
`Context.BoundSession.DisconnectAsync(ct)`.

---

## `DisconnectAsync` (inside Actor code, bound session)

Disconnects the client connection bound to this Actor. Call it through
`Context.BoundSession.DisconnectAsync(ct)`.

```csharp
await Context.BoundSession.DisconnectAsync(ct);
```

**Options.** This call has no modifiers — it takes only a `CancellationToken`.

**Completion.** Disconnects the bound session.

**When to use it.** Use it from Actor-side code when a specific client connection no longer
needs to be kept. To disconnect from the Session side directly, use the `CloseAsync` entry.

---

The full basis is the
[STREAM session exact interface](../../common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md) and the
[Bound STREAM session exact interface](../../common/spec/server/languages/dotnet/interfaces/07-bound-stream-session.ko.md)
(both Korean-only).
