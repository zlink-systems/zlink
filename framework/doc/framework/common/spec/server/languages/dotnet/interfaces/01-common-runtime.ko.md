# .NET common runtime 공개 인터페이스

[.NET 언어별 interface 목차](README.ko.md)

## 1. 공통 metadata와 call

Handler metadata는 변경할 수 없는 snapshot이다.

받은 `ZLinkMessage`에서 처음 호출한 typed `Decode<T>()`가 값 또는 실패를 확정한다. 이후
`Decode<T>()`는 같은 결과를 사용하며 serializer를 다시 호출하지 않는다. 다른 `T`가 첫 값을
받을 수 없으면 type cast가 실패한다. `Decode<ReadOnlyMemory<byte>>()`는 Framework가 소유한
읽기 전용 view를 반환하고, `Decode<byte[]>()`는 호출자가 소유하는 새 복사본을 반환한다. 이 두
raw 접근은 typed 결과를 확정하지 않는다.

```csharp
public sealed class ZLinkMessage
{
    public static ZLinkMessage Empty { get; }
    public string? ContentType { get; }
    public bool IsEmpty { get; }
    public ZlinkStreamCodec? StreamCodec { get; }
    public static ZLinkMessage From<T>(T value);
    public T Decode<T>();
}

public sealed class ZLinkMessageMetadata
{
    public ZLinkMessageMetadata(
        IReadOnlyDictionary<string, string> values);
    public static ZLinkMessageMetadata Empty { get; }
    public IReadOnlyDictionary<string, string> Values { get; }
    public string? Find(string key);
}

public interface IZLinkSendCall : IZLinkMetadataCall<IZLinkSendCall>
{
    ValueTask Async(
        CancellationToken cancellationToken = default);
    void Submit();   // 동기 blocking; runtime 실행 문맥에서 InvalidOperation (F2-a)
}

public interface IZLinkRequestCall : IZLinkMetadataCall<IZLinkRequestCall>
{
    IZLinkRequestCall Timeout(TimeSpan timeout);
    ValueTask<TReply> Async<TReply>(
        CancellationToken cancellationToken = default);
    TReply Submit<TReply>();   // 동기 blocking; runtime 실행 문맥에서 InvalidOperation (F2-a)
    ValueTask<TReply> Yield<TReply>(
        CancellationToken cancellationToken = default);
}

public interface IZLinkPublishCall : IZLinkMetadataCall<IZLinkPublishCall>
{
    ValueTask Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkFanoutPublishCall
{
    ValueTask Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkWorkerCall<TResult>
{
    IZLinkWorkerCall<TResult> Timeout(TimeSpan timeout);
    void Submit(CancellationToken cancellationToken = default);
    ValueTask<TResult> Async(CancellationToken cancellationToken = default);
    ValueTask<TResult> Yield(CancellationToken cancellationToken = default);
}

public interface IZLinkWorkerOptions
{
    int MinThreads { get; set; }
    int MaxThreads { get; set; }
    TimeSpan IdleTimeout { get; set; }
}
```

One-way admission, timeout과 terminal completion은
[Submit과 completion](../../../01-execution/01-submit-and-completion.ko.md)이 정한다.
.NET `Async()`는 결과 없는 `ValueTask`를 반환하며 실패를 exceptional completion으로 전달한다.

One-way send timeout과 기본값은
[Submit과 completion](../../../01-execution/01-submit-and-completion.ko.md)이 정한다.

`IZLinkPublishCall`은 결과 없는 `ValueTask`를 반환한다. Logical Multicast의 완료 경계는
[Submit과 completion §6](../../../01-execution/01-submit-and-completion.ko.md)이 정한다.

`CancellationToken`의 취소와 admission·timeout·shutdown의 terminal 경쟁은
[Submit과 completion](../../../01-execution/01-submit-and-completion.ko.md)이 정한다.
.NET은 취소를 cancelled `ValueTask`로 전달한다.

잘못된 인자·handle·상태, 중복 terminal과 이미 사용한 reply token은 .NET exceptional completion으로
처리한다. Timeout이나 cancellation 뒤에는 operation을 자동으로 다시 제출하지 않는다.
`IZLinkMetadataCall<TSelf>`의 정확한 시그니처와 1024-byte 상한은
[Topology configuration §6](03-configuration-topology.ko.md#6-메시징-metadata)이 소유한다. 같은 key를 여러 번 설정하면
마지막 값이 전송된다. Reply는 request metadata를 자동 복사하지 않는다.

Worker call의 `Submit`, `Async`와 `Yield`는
[비동기 실행 정책 §1.2](../../../01-execution/02-handler-turn-and-execution-gate.ko.md)의 완료 의미를 따른다.
Worker option은 host가 시작되기 전에만 설정할 수 있다.

`Yield` terminal은 `RequestToChannel`, `RequestToSpot`, `RequestToActor`, `RunIoWorker`,
`RunCpuWorker`와 Actor·Spot create·get-or-create call에 있다. 유효 문맥과 제출 전 오류는
[Handler turn과 execution gate](../../../01-execution/02-handler-turn-and-execution-gate.ko.md)가 정한다.

`SpotWide` member Actor의 `Yield` 중 gate와 Actor claim 처리는
[Handler turn과 execution gate](../../../01-execution/02-handler-turn-and-execution-gate.ko.md)가 정한다.

Assembly scan에서 사용하는 최소 attribute 표면은 다음과 같다.

```csharp
[AttributeUsage(AttributeTargets.Class, AllowMultiple = true)]
public sealed class ZLinkHandlerGroupAttribute(string groupName) : Attribute
{
    public string GroupName { get; } = groupName;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkRequestAttribute : Attribute
{
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkSendAttribute : Attribute
{
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkPublishAttribute : Attribute
{
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
public sealed class ZLinkPacketAttribute(string packetName) : Attribute
{
    public string PacketName { get; } = packetName;
}
```

Root에 등록한 assembly에서 method attribute를 찾으며, `ZLinkHandlerGroupAttribute`는 해당 handler가
참여하는 handler group을 지정한다. Method의 `PacketName`을 생략하면 message type의
`ZLinkPacketAttribute`를 확인하고, 그것도 없으면 type 이름을 사용한다. Packet name은 등록할 때 한 번
결정되며 codec 선택으로 바뀌지 않는다.
## 4. Handler attribute

```csharp
[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkSpotRequestAttribute : Attribute
{
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotPacketHandlerAttribute(string packetName) : Attribute
{
    public string PacketName { get; } = packetName;
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotRequestHandlerAttribute(string packetName) : Attribute
{
    public string PacketName { get; } = packetName;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkSpotSubscriptionAttribute : Attribute
{
    public ZLinkSpotSubscriptionAttribute(
        string spotNodeName,
        string channelName,
        string topic);
    public string SpotNodeName { get; }
    public string ChannelName { get; }
    public string Topic { get; }
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotSubscriptionHandlerAttribute : Attribute
{
    public ZLinkSpotSubscriptionHandlerAttribute(
        string channelName,
        string topic);
    public string ChannelName { get; }
    public string Topic { get; }
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkSpotActorSendAttribute : Attribute
{
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotActorSendHandlerAttribute(string packetName) : Attribute
{
    public string PacketName { get; } = packetName;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkSpotActorRequestAttribute : Attribute
{
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotActorRequestHandlerAttribute(string packetName) : Attribute
{
    public string PacketName { get; } = packetName;
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotTimerHandlerAttribute(
    string name,
    double periodMilliseconds) : Attribute
{
    public string Name { get; } = name;
    public double PeriodMilliseconds { get; } = periodMilliseconds;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkStreamPacketAttribute : Attribute;
```
