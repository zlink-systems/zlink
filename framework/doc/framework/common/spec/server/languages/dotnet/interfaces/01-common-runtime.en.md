# .NET Common Runtime Public Interface

[.NET exact interface table of contents](README.en.md)

## 1. Common Metadata And Call

Handler metadata is an immutable snapshot.

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

A one-way call's `Async()` doesn't produce a normal-completion value.
Normal completion means the source-local queue the operation family
defines accepted the message. It doesn't wait for remote handler
execution, subscriber receipt, remote Spot queue admission, or
application callback completion. If queue capacity is insufficient, it
waits for a capacity signal up to that family's send timeout, and submits
the message exactly once if room opens up within the deadline. Timeout
completes exceptionally with `DeadlineExceeded`, a route break with
`Unavailable`, and runtime shutdown with `ShuttingDown`. Absence of an
Actor/Spot/Mesh/session target uses `NotFound`. If `CancellationToken` is
triggered first, it completes with a cancelled `ValueTask`.

Each one-way call uses the send timeout set on its public configuration.
The default when there's no public setting is 1 second.

Logical Multicast's `IZLinkPublishCall` starts the publish and completes
normally with no return value once it secures source-local execution
capacity within the send timeout. After starting, an individual target
failure doesn't turn into an overall failure and isn't automatically
retried. Per-target admission/failure results aren't returned or
aggregated into monitoring, and it completes normally even with no
targets.

If `CancellationToken` is triggered before admission, it completes
exactly once with a cancelled `ValueTask`. Pre-cancellation doesn't start
runtime admission. If admission/timeout/[shutdown](../../../../01-glossary.en.md#shutdown)
and cancellation race, only one atomic terminal winner completes, and
late admission isn't created after a timeout or cancellation. For
[Logical Multicast](../../../../01-glossary.en.md#logical-multicast), only
cancellation before publish starts blocks the operation from starting.
Once publish has started, submission to the selected target set proceeds
to completion.

An invalid argument/handle/state, a duplicate terminal, and an
already-used reply token are handled as .NET exceptional completion. An
operation isn't automatically resubmitted after a timeout or
cancellation. The exact signature of `IZLinkMetadataCall<TSelf>` and the
1024-byte upper bound are owned by
[Topology Configuration §6](03-configuration-topology.en.md#6-messaging-metadata).
Setting the same key multiple times sends the last value. A reply doesn't
automatically copy request metadata.

The worker call's `Submit`, `Async`, and `Yield` follow the completion
semantics of
[Async Execution Policy §1.2](../../../../05-async-execution-policy.en.md#12-worker-offload).
Worker options can only be set before the host starts.

The `Yield` terminal only exists on `RequestToChannel`,
`RequestToSpot`, `RequestToActor`, `RunIoWorker`, `RunCpuWorker`, and the
Actor/Spot create/get-or-create call. It isn't provided for Actor join,
Node direct request, send, publish, timer registration, close, and
destroy. Even for a common request/worker/create call, the runtime checks
the current execution context before operation submit. If it isn't a
`SpotWide` User Spot or Instance Spot application handler, it completes
with `InvalidOperation` without outbound admission, queue change, or gate
return.

If a `SpotWide` member Actor yields, the Actor queue claim is kept and
only the User Spot gate is returned. The terminal continuation
re-acquires the same gate, finishes the current Actor job, and then
releases the Actor claim. The same Actor's next job doesn't start before
that. `Yield` isn't allowed on a `PerActor` User Spot or Entry Spot.

The minimal attribute surface used for assembly scanning is as follows.

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

It looks for method attributes in the assemblies registered on the root,
and `ZLinkHandlerGroupAttribute` specifies the handler group that handler
participates in. If a method's `PacketName` is omitted, it checks the
message type's `ZLinkPacketAttribute`, and if that's also absent, uses
the type name. Packet name is decided once at registration time and
doesn't change with codec selection.
## 4. Handler Attribute

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
