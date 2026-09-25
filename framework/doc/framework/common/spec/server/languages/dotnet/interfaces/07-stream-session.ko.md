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
 public IZLinkSessionActor? Actor { get; } // packet의 Actor slot이 가리키는 현재 binding. 없으면 null
}
```

`IZLinkSessionReplyCall`은 현재 request sequence와 one-shot reply token을 전송 전에 검증한다. 유효한 첫
terminator는 transport를 시작하기 전에 token을 원자적으로 claim하고 소비한다. 같은 token에서 만든 두 call이
경쟁하면 claim에 실패한 call은 transport를 시도하지 않고 exceptional completion으로 끝난다. Send packet에서
만든 reply, 이미 사용한 token과 중복 submit도 같은 방식으로 거부한다. Token을 소비한 call이 timeout,
`DeadlineExceeded` 또는 cancellation로 끝나도 token을 다시 사용할 수 없다. 유효한 reply는 STREAM socket send
timeout만 admission deadline으로 사용한다. Caller request timeout은 wire로 전달되지 않으므로 reply [deadline](../../../00-foundation/02-glossary.ko.md#deadline)으로
사용하지 않으며, timeout이나 cancellation 뒤에는 late reply를 보내지 않는다.

`IZLinkSessionSendCall.Timeout(...)`은 이 send의 admission 대기만 줄인다. 생략하면 STREAM socket
`SendTimeout`을 사용하고 지정하면 두 값 중 짧은 값을 사용하므로 socket timeout을 늘릴 수 없다. 양수
`1..Int32.MaxValue` milliseconds 범위로 올림한 값만 허용한다. 만료되면 `DeadlineExceeded`로 terminal-once
완료하고 이후 admission이나 replay를 시작하지 않는다. `CancellationToken`은 기존 .NET cancellation
계약을 유지하며 reply call에는 이 modifier를 제공하지 않는다.

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
