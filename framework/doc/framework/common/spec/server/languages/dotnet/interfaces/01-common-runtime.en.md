# .NET Common Runtime Public Interface

[.NET per-language interface table of contents](README.en.md)

## 1. Common Metadata And Call

Handler metadata is an immutable snapshot.

The first typed `Decode<T>()` on a received `ZLinkMessage` fixes either a value or a
failure. Later `Decode<T>()` calls reuse that outcome and do not invoke the serializer
again. A type cast fails if a later `T` cannot accept the first value.
`Decode<ReadOnlyMemory<byte>>()` returns a read-only view owned by the framework, while
`Decode<byte[]>()` returns a new caller-owned copy. Neither raw access fixes the typed
outcome.

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
 void Submit();   // synchronous blocking; InvalidOperation in a runtime execution context (F2-a)
}

public interface IZLinkRequestCall : IZLinkMetadataCall<IZLinkRequestCall>
{
 IZLinkRequestCall Timeout(TimeSpan timeout);
 ValueTask<TReply> Async<TReply>(
 CancellationToken cancellationToken = default);
 TReply Submit<TReply>();   // synchronous blocking; InvalidOperation in a runtime execution context (F2-a)
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

[Submit and completion](../../../01-execution/01-submit-and-completion.en.md)
defines one-way admission, timeout, and terminal completion.
.NET `Async()` returns a resultless `ValueTask` and reports failure by exceptional completion.

[Submit and completion](../../../01-execution/01-submit-and-completion.en.md)
defines the one-way send timeout and its default.

`IZLinkPublishCall` returns a resultless `ValueTask`.
[Submit and completion §6](../../../01-execution/01-submit-and-completion.en.md)
defines the Logical Multicast completion boundary.

[Submit and completion](../../../01-execution/01-submit-and-completion.en.md)
defines cancellation and the terminal race with admission, timeout, and shutdown.
.NET reports cancellation with a cancelled `ValueTask`.

An invalid argument/handle/state, a duplicate terminal, and an
already-used reply token are handled as .NET exceptional completion. An
operation isn't automatically resubmitted after a timeout or
cancellation. The signature of `IZLinkMetadataCall<TSelf>` and the
1024-byte upper bound are owned by
[Topology Configuration §6](03-configuration-topology.en.md#6-messaging-metadata).
Setting the same key multiple times sends the last value. A reply doesn't
automatically copy request metadata.

The worker call's `Submit`, `Async`, and `Yield` follow the completion
semantics of
[Async Execution Policy §1.2](../../../01-execution/02-handler-turn-and-execution-gate.en.md).
Worker options can only be set before the host starts.

The `Yield` terminal exists on `RequestToChannel`, `RequestToSpot`, `RequestToActor`,
`RunIoWorker`, `RunCpuWorker`, and Actor/Spot create/get-or-create calls.
[Handler turn and execution gate](../../../01-execution/02-handler-turn-and-execution-gate.en.md)
defines eligibility and pre-submission errors.

[Handler turn and execution gate](../../../01-execution/02-handler-turn-and-execution-gate.en.md)
defines gate and Actor-claim handling when a `SpotWide` member Actor yields.

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
