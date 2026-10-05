# .NET STREAM server session 공개 인터페이스

[.NET 언어별 interface 목차](README.ko.md)

## 1. STREAM server session

STREAM session은 lifecycle과 typed packet handler를 소유한다. Framework 내부 recv loop는
Core의 raw STREAM part를 수신해 managed queue에 넣은 뒤 application callback을 실행한다.
Transport callback으로 queue admission을 우회하지 않는다.

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
 public IZLinkSessionActor? Actor { get; } // packet의 Actor slot이 가리키는 현재 binding. 없으면 null
}
```

`IZLinkSessionReplyCall`은 현재 request sequence와 one-shot reply token을 전송 전에 검증한다. 유효한 첫
terminator는 transport를 시작하기 전에 token을 원자적으로 claim하고 소비한다. 같은 token에서 만든 두 call이
경쟁하면 claim에 실패한 call은 transport를 시도하지 않고 exceptional completion으로 끝난다. Send packet에서
만든 reply, 이미 사용한 token과 중복 submit도 같은 방식으로 거부한다. Token을 소비한 call이 실패로 끝나도
token을 다시 사용할 수 없다. Send·reply·relay의 대기에는 시간 상한과 `CancellationToken`이 없다
([Submit과 completion §7](../../../01-execution/01-submit-and-completion.ko.md#7-one-way-send의-대기-종료와-classic-fanout-send-timeout)). Caller request timeout은 wire로 전달되지 않으며 reply의 대기를 끝내지 않는다.

`OnActorBindingReplacedAsync(...)`, `RelayAsync(...)`와 `NotifyDisconnectedAsync(...)`는 .NET callback·call 표면이다.
Binding 교체, relay 완료, disconnect와 relocation route 갱신은
[Session–Actor binding](../../../04-session/02-session-actor-binding.ko.md)이 정한다.
`IZLinkSessionActor.Ref`의 타입은 `ActorRef`다.

## 2. STREAM transport handle

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

`IZLinkStream`은 session callback에서 transport-facing operation을 제공한다. Bound session과 typed call은
이 interface와 별도 책임을 가진다.
