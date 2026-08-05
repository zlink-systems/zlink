# .NET STREAM Server Session Public Interface

[.NET exact interface table of contents](README.en.md)

## 1. STREAM Server Session

A STREAM session owns lifecycle and typed packet handlers. The
framework's internal recv loop receives Core's raw STREAM parts, puts
them on a managed queue, and then runs the application callback. Queue
admission isn't bypassed by a transport callback.

```csharp
public interface IZLinkSession
{
    IZLinkSessionContext Context { get; }
    void Configure() { }
    ValueTask OnConnectedAsync(CancellationToken cancellationToken);
    ValueTask OnDisconnectedAsync(CancellationToken cancellationToken);
    ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken cancellationToken);
    ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }
}

public interface IZLinkSessionContext
{
    string SessionId { get; }
    RoutingId? RoutingId { get; }
    string? LocalAddr { get; }
    string? RemoteAddr { get; }
    IZLinkSessionClient Client { get; }
    IZLinkSessionActors Actors { get; }
    IZLinkSessionHandlerRegistry Handlers { get; }
    ValueTask CloseAsync();
}

public interface IZLinkSessionHandlerRegistry
{
    void AddHandler<THandler>() where THandler : class;
    void AddHandler<THandler>(string packetName) where THandler : class;
    ValueTask<bool> TryHandleAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken = default);
}

public interface IZLinkSessionPacketHandler<in TSessionContext, TMessage>
{
    ValueTask HandleAsync(
        TSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        TMessage message,
        CancellationToken cancellationToken);
}

public interface IZLinkSessionClient
{
    IZLinkSessionSendCall Send<TMessage>(TMessage message);
    IZLinkSessionReplyCall Reply<TMessage>(TMessage message);
}

public interface IZLinkSessionSendCall
    : IZLinkMetadataCall<IZLinkSessionSendCall>
{
    IZLinkSessionSendCall Compress();
    ValueTask Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkSessionReplyCall
{
    IZLinkSessionReplyCall Compress();
    ValueTask Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkSessionActors
{
    IReadOnlyCollection<IZLinkSessionActor> Bound { get; }
    ValueTask<IZLinkSessionActor> BindAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default);
    ValueTask<IZLinkSessionActor> BindOrGetAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default);
    IZLinkSessionActor? Find(string actorId);
}

public interface IZLinkSessionActor
{
    string ActorId => Ref.ActorId;
    ActorRef Ref { get; }
    ValueTask RelayAsync(
        ZLinkMessage payload,
        CancellationToken cancellationToken = default);
    ValueTask NotifyDisconnectedAsync(
        CancellationToken cancellationToken = default);
}

public enum ZLinkStreamSessionError
{
    Internal = 0,
    TransportError = 1,
    HandshakeFailed = 2
}

public readonly record struct ZLinkStreamError(
    ZLinkStreamSessionError Error,
    string? Message);

public sealed class ZLinkSessionDispatchContext
{
    public ZLinkSessionDispatchContext(
        string packetName,
        ZLinkMessageMetadata? metadata = null,
        bool canReply = false) { }
    public string PacketName { get; }
    public ZLinkMessageMetadata Metadata { get; }
    public bool CanReply { get; }
}
```

`IZLinkSessionReplyCall` validates the current request sequence and
one-shot reply token before sending. A valid first terminator atomically
claims and consumes the token before starting transport. If two calls
created from the same token race, the one that fails the claim doesn't
attempt transport and ends with exceptional completion. A reply created
from a send packet, an already-used token, and a duplicate submit are
rejected the same way. Even if the call that consumed the token ends with
timeout, `DeadlineExceeded`, or cancellation, the token can't be used
again. A valid reply only uses the STREAM socket send timeout as the
admission deadline. Since the caller request timeout isn't delivered over
the wire, it isn't used as the reply
[deadline](../../../../01-glossary.en.md#deadline), and no late reply is
sent after a timeout or cancellation.

After bind, `RelayAsync(...)` and `NotifyDisconnectedAsync(...)` use the
per-Actor binding. A physical disconnect is notified by the framework to
every current binding, running the Spot callback at most once per exact
binding identity. `NotifyDisconnectedAsync(...)` is a logical
notification while the connection is kept, and waits for the callback
terminal. Relocation keeps the same ObjectGeneration and only updates
that Actor's binding. A different Actor binding of the same Session and
the physical STREAM connection aren't changed.

`RelayAsync(...)` is a one-way operation that completes normally once the
Actor relay accepts source-local admission. A request reply is submitted
explicitly through the session callback's `IZLinkSessionClient.Reply(...)`.

Packet and lifecycle callbacks of the same session run serially.
Handshake and node-scope errors are reported through runtime monitoring
and aren't delivered to `OnErrorAsync(...)`.

Session binding fixes the exact incarnation of `ActorRef.ActorId +
ObjectGeneration` once. The MeshName/NodeRid of the Ref submitted at bind
is used as the initial control route snapshot. If there's no mapping,
`NotFound`; if the current generation differs, `InvalidOperation`; if in
pre-commit seal, `Unavailable` — the framework doesn't find a different
ref in the Store and hidden-retry the same bind operation. Once an Actor
relocation commits after bind, the runtime updates the binding route and
the current location snapshot `IZLinkSessionActor.Ref` returns together.
The new snapshot has the same ActorId/ObjectGeneration and the target
MeshName/NodeRid. The application doesn't rebind to learn about
relocation. An overload taking a local `IZLinkActor` isn't provided.

## 2. STREAM Transport Handle

```csharp
public interface IZLinkStream
{
    string SessionId { get; }
    RoutingId? RoutingId { get; }
    string? LocalAddr { get; }
    string? RemoteAddr { get; }
    bool Write(
        ZLinkMessage payload,
        SendFlags flags = SendFlags.None);
    ValueTask CloseAsync();
}

public interface IZLinkMessageMetadataPolicy
{
    bool CanForward(string key);
}
```

`IZLinkStream` provides transport-facing operations in the session
callback. Bound session and the typed call have a separate
responsibility from this interface.
