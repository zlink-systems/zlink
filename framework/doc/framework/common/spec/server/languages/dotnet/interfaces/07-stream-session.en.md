# .NET STREAM Server Session Public Interface

[.NET per-language interface table of contents](README.en.md)

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
 ValueTask OnActorBindingReplacedAsync(
 string actorId,
 CancellationToken cancellationToken)
 {
 return ValueTask.CompletedTask;
 }
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
 IZLinkSessionSendCall Timeout(TimeSpan timeout);
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
 bool canReply = false,
 IZLinkSessionActor? actor = null) { }
 public string PacketName { get; }
 public ZLinkMessageMetadata Metadata { get; }
 public bool CanReply { get; }
 public IZLinkSessionActor? Actor { get; } // the current binding the packet's Actor slot points at; null when there is none
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
[deadline](../../../00-foundation/02-glossary.en.md#deadline), and no late reply is
sent after a timeout or cancellation.

`IZLinkSessionSendCall.Timeout(...)` only shortens this send's admission
wait. Omission uses the STREAM socket `SendTimeout`; specifying it uses the
shorter of the two, so it cannot extend the socket timeout. Only a positive
value that rounds up into `1..Int32.MaxValue` milliseconds is valid. Expiry
completes terminal-once as `DeadlineExceeded` and does not start later
admission or replay. `CancellationToken` keeps the existing .NET cancellation
contract, and the reply call doesn't provide this modifier.

`OnActorBindingReplacedAsync(...)`, `RelayAsync(...)`, and `NotifyDisconnectedAsync(...)`
are the .NET callback and call surface. [Session–Actor binding](../../../04-session/02-session-actor-binding.en.md)
defines replacement, relay completion, disconnect, and relocation route updates.
`IZLinkSessionActor.Ref` has type `ActorRef`.

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
