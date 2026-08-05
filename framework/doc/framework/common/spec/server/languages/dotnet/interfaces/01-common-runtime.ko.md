# .NET common runtime 공개 인터페이스

[.NET exact interface 목차](README.ko.md)

## 1. 공통 metadata와 call

Handler metadata는 변경할 수 없는 snapshot이다.

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
}

public interface IZLinkRequestCall : IZLinkMetadataCall<IZLinkRequestCall>
{
    IZLinkRequestCall Timeout(TimeSpan timeout);
    ValueTask<TReply> Async<TReply>(
        CancellationToken cancellationToken = default);
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
    int MaxQueueLength { get; set; }
}
```

One-way call의 `Async()`는 정상 완료 값을 만들지 않는다. 정상 완료는 operation family가 정의한
source-local queue가 message를 수락했다는 뜻이다. Remote handler 실행, subscriber 수신, remote Spot queue
수락과 application callback 완료는 기다리지 않는다. Queue capacity가 부족하면 해당 family의 send timeout까지
capacity signal을 기다리고, deadline 안에 공간이 생기면 message를 정확히 한 번 제출한다. Timeout은
`DeadlineExceeded`, route 단절은 `Unavailable`, runtime 종료는 `ShuttingDown`으로 exceptional
completion한다. Actor·Spot·Mesh·session target 부재는 `NotFound`를 사용한다.
`CancellationToken`이 먼저 확정되면 cancelled `ValueTask`로 완료한다.

각 one-way call은 해당 public configuration에 설정한 send timeout을 사용한다. 공개 설정이 없을 때의
기본값은 1초다.

Logical Multicast의 `IZLinkPublishCall`은 source-local 실행 용량을 send timeout 안에 확보하면 publish를
시작하고 결과값 없이 정상 완료한다. 시작한 뒤에는 개별 target 실패를 전체 실패로 바꾸거나 자동으로
재시도하지 않는다. Target별 수락·실패 결과는 반환하거나 monitoring에 집계하지 않으며 target이 없어도
정상 완료한다.

`CancellationToken`이 admission보다 먼저 확정되면 cancelled `ValueTask`로 한 번만 완료한다.
Pre-cancellation은 runtime admission을 시작하지 않는다. Admission·timeout·[shutdown](../../../../01-glossary.ko.md#shutdown)과
cancellation이 경쟁하면 원자 terminal winner 하나만 완료하고 timeout이나 cancellation 뒤에 late admission을
만들지 않는다. [Logical Multicast](../../../../01-glossary.ko.md#logical-multicast)는 publish가 시작되기 전 cancellation만 operation 시작을 막는다.
Publish가 시작된 뒤에는 선택한 target 집합에 대한 제출을 끝까지 진행한다.

잘못된 인자·handle·상태, 중복 terminal과 이미 사용한 reply token은 .NET exceptional completion으로
처리한다. Timeout이나 cancellation 뒤에는 operation을 자동으로 다시 제출하지 않는다.
`IZLinkMetadataCall<TSelf>`의 정확한 시그니처와 1024-byte 상한은
[Topology configuration §6](03-configuration-topology.ko.md#6-메시징-metadata)이 소유한다. 같은 key를 여러 번 설정하면
마지막 값이 전송된다. Reply는 request metadata를 자동 복사하지 않는다.

Worker call의 `Submit`, `Async`와 `Yield`는
[비동기 실행 정책 §1.2](../../../../05-async-execution-policy.ko.md#12-worker-offload)의 완료 의미를 따른다.
Worker option은 host가 시작되기 전에만 설정할 수 있다.

`Yield` terminal은 `RequestToChannel`, `RequestToSpot`, `RequestToActor`, `RunIoWorker`, `RunCpuWorker`와
Actor·Spot create·get-or-create call에만 존재한다. Actor join, Node direct request, send, publish, timer 등록,
close와 destroy에는 제공하지 않는다. 공통 request·worker·create call이더라도 Runtime은
operation submit 전에 current execution context를 확인한다. `SpotWide` User Spot 또는 Instance Spot
application handler가 아니면 outbound admission, queue 변경과 gate 반환 없이
`InvalidOperation`으로 완료한다.

`SpotWide` member Actor가 `Yield`하면 Actor queue claim은 유지하고 User Spot gate만 반환한다. Terminal
continuation은 같은 gate를 다시 얻어 현재 Actor job을 끝낸 뒤 Actor claim을 해제한다. 같은 Actor의 다음
job은 그 전에 시작하지 않는다. `PerActor` User Spot과 Entry Spot에서는 `Yield`를 허용하지 않는다.

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
