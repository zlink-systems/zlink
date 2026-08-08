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

`IZLinkSessionSendCall.Timeout(...)`은 이 send의 admission 대기만 줄인다. 생략하면 STREAM socket
`SendTimeout`을 사용하고 지정하면 두 값 중 짧은 값을 사용하므로 socket timeout을 늘릴 수 없다. 양수
`1..Int32.MaxValue` milliseconds 범위로 올림한 값만 허용한다. 만료되면 `DeadlineExceeded`로 terminal-once
완료하고 이후 admission이나 replay를 시작하지 않는다. `CancellationToken`은 기존 .NET cancellation
계약을 유지하며 reply call에는 이 modifier를 제공하지 않는다.

`OnActorBindingReplacedAsync(...)`는 같은 Actor가 새 session에 bind된 경우 이전 session에서 한 번 실행되는
선택 callback이다. Application은 이 callback에서 `Context.Client.Send(...)`로 client 안내를 보낼 수 있지만
`Context.CloseAsync()`를 호출하지 않는다. Callback이 성공 또는 실패로 terminal이 되면 Framework가 `100 ms`
뒤 connection을 닫는다. Outbound queue가 먼저 비어도 이 시간을 줄이지 않는다. 새 bind는 이 callback이나
close를 기다리지 않는다.

| 구현 차이 | 현재 상태 |
|---|---|
| Session Actor binding 교체 | .NET runtime에는 command 51 송수신, 이 callback과 non-blocking 100 ms close timer가 아직 없다. |

Bind 뒤 `RelayAsync(...)`와 `NotifyDisconnectedAsync(...)`는 Actor별 binding을 사용한다. Physical disconnect는
Framework가 current binding 전체에 통지하고 exact binding identity마다 Spot callback을 최대 한 번 실행한다.
`NotifyDisconnectedAsync(...)`는 connection이 유지된 상태의 logical notification이며 callback terminal까지
기다린다. Exact binding callback은 최대 한 번 실행하고 terminal 뒤 해당 binding을 tombstone으로
확정하여 제거한다. Physical STREAM connection과 Actor·Spot membership은 유지한다. 새 public Unbind API는
제공하지 않는다. Rebind는 새 identity를 current로 등록한 즉시 완료되며 이전 session의 처리를 기다리지
않는다. 이전 exact session의 `OnActorBindingReplacedAsync(...)`에서 client 안내를 보낼 수 있다. Callback이
성공 또는 실패로 terminal이 되면 Framework가 `100 ms` 뒤 connection을 닫는다. Callback이나 close 실패는 새 binding을
제거하거나 이전 binding을 복원하지 않는다.
Relocation은 같은 ObjectGeneration을 유지하며 해당 Actor의 binding route만 갱신하는 동작이므로 rebind가
아니고 disconnect callback을 실행하지 않는다. 같은 Session의
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
