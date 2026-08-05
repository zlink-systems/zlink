# .NET STREAM server session 공개 인터페이스

[.NET exact interface 목차](README.ko.md)

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

`IZLinkSessionReplyCall`은 현재 request sequence와 one-shot reply token을 전송 전에 검증한다. 유효한 첫
terminator는 transport를 시작하기 전에 token을 원자적으로 claim하고 소비한다. 같은 token에서 만든 두 call이
경쟁하면 claim에 실패한 call은 transport를 시도하지 않고 exceptional completion으로 끝난다. Send packet에서
만든 reply, 이미 사용한 token과 중복 submit도 같은 방식으로 거부한다. Token을 소비한 call이 timeout,
`DeadlineExceeded` 또는 cancellation로 끝나도 token을 다시 사용할 수 없다. 유효한 reply는 STREAM socket send
timeout만 admission deadline으로 사용한다. Caller request timeout은 wire로 전달되지 않으므로 reply [deadline](../../../../01-glossary.ko.md#deadline)으로
사용하지 않으며, timeout이나 cancellation 뒤에는 late reply를 보내지 않는다.

Bind 뒤 `RelayAsync(...)`와 `NotifyDisconnectedAsync(...)`는 Actor별 binding을 사용한다. Physical disconnect는
Framework가 current binding 전체에 통지하고 exact binding identity마다 Spot callback을 최대 한 번 실행한다.
`NotifyDisconnectedAsync(...)`는 connection이 유지된 상태의 logical notification이며 callback terminal까지
기다린다. Relocation은 같은 ObjectGeneration을 유지하며 해당 Actor의 binding만 갱신한다. 같은 Session의
다른 Actor binding과 physical STREAM connection은 변경하지 않는다.

`RelayAsync(...)`는 Actor relay가 source-local admission을 수락하면 정상 완료하는 one-way operation이다.
Request reply는 session callback의 `IZLinkSessionClient.Reply(...)`로 명시적으로 제출한다.

같은 session의 packet과 lifecycle callback은 직렬로 실행한다. Handshake와 node 범위 오류는 runtime
monitoring으로 보고하며 `OnErrorAsync(...)`에 전달하지 않는다.

Session binding은 `ActorRef.ActorId + ObjectGeneration`의 exact incarnation을 한 번 고정한다. Bind에
제출한 Ref의 MeshName·NodeRid는 최초 control route snapshot으로 사용한다. Mapping이 없으면
`NotFound`, current generation이 다르면 `InvalidOperation`, pre-commit seal 중이면
`Unavailable`이다. Framework는 Store에서 다른 ref를 찾아 같은 bind operation을 hidden retry하지 않는다.
Bind 뒤 Actor relocation이 commit되면 runtime은 binding route와 `IZLinkSessionActor.Ref`가 반환하는
current location snapshot을 함께 갱신한다. 새 snapshot은 같은 ActorId·ObjectGeneration과 target
MeshName·NodeRid를 가진다. Application은 relocation을 알기 위해 rebind하지 않는다. Local
`IZLinkActor`를 받는 overload는 제공하지 않는다.

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
