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
 ValueTask Async();
}

public interface IZLinkSessionReplyCall
{
 IZLinkSessionReplyCall Compress();
 ValueTask Async();
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
 ZLinkMessage payload);
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
rejected the same way. Even if the call that consumed the token ends with a
failure, the token can't be used again. The wait of a send, reply, or relay
has no time limit and no `CancellationToken`
([Submit and completion §7](../../../01-execution/01-submit-and-completion.en.md#7-one-way-send-wait-termination-and-classic-fanout-send-timeout)). The caller request timeout isn't delivered
over the wire and doesn't end the reply's wait.

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
