namespace Zlink.Framework.Contracts.Handlers;

[AttributeUsage(AttributeTargets.Class, AllowMultiple = true)]
public sealed class ZLinkHandlerGroupAttribute(string groupName) : Attribute
{
    public string GroupName { get; } = groupName;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkRequestAttribute : Attribute
{
    /// <summary>
    /// Optional packet-name override for method-scanned handlers. When omitted,
    /// the framework derives the packet name from the message type.
    /// </summary>
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkSendAttribute : Attribute
{
    /// <summary>
    /// Optional packet-name override for method-scanned handlers. When omitted,
    /// the framework derives the packet name from the message type.
    /// </summary>
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkPublishAttribute : Attribute
{
    /// <summary>
    /// Optional packet-name override for method-scanned handlers. When omitted,
    /// the framework derives the packet name from the message type.
    /// </summary>
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
public sealed class ZLinkPacketAttribute(string packetName) : Attribute
{
    public string PacketName { get; } = packetName;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkSpotRequestAttribute : Attribute
{
    /// <summary>
    /// Optional packet-name override for method-scanned spot request handlers.
    /// </summary>
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotPacketHandlerAttribute(string packetName) : Attribute
{
    /// <summary>
    /// Required packet name for class-scanned spot packet handlers.
    /// </summary>
    public string PacketName { get; } = packetName;
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotRequestHandlerAttribute(string packetName) : Attribute
{
    /// <summary>
    /// Required packet name for class-scanned spot request handlers.
    /// </summary>
    public string PacketName { get; } = packetName;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkSpotSubscriptionAttribute(
    string spotNodeName,
    string channelName,
    string topic) : Attribute
{
    public string SpotNodeName { get; } = spotNodeName;

    public string ChannelName { get; } = channelName;

    public string Topic { get; } = topic;
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotSubscriptionHandlerAttribute(
    string channelName,
    string topic) : Attribute
{
    public string ChannelName { get; } = channelName;

    public string Topic { get; } = topic;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkSpotActorSendAttribute : Attribute
{
    /// <summary>
    /// Optional packet-name override for method-scanned actor send handlers.
    /// </summary>
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotActorSendHandlerAttribute(string packetName) : Attribute
{
    /// <summary>
    /// Required packet name for class-scanned actor send handlers.
    /// </summary>
    public string PacketName { get; } = packetName;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkSpotActorRequestAttribute : Attribute
{
    /// <summary>
    /// Optional packet-name override for method-scanned actor request handlers.
    /// </summary>
    public string? PacketName { get; init; }
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotActorRequestHandlerAttribute(string packetName) : Attribute
{
    /// <summary>
    /// Required packet name for class-scanned actor request handlers.
    /// </summary>
    public string PacketName { get; } = packetName;
}

[AttributeUsage(AttributeTargets.Class)]
public sealed class ZLinkSpotTimerHandlerAttribute(string name, double periodMilliseconds) : Attribute
{
    public string Name { get; } = name;

    /// <summary>
    /// Timer period in milliseconds. Attribute constructor arguments must be
    /// compile-time constants, so the contract uses a numeric value instead of
    /// TimeSpan.
    /// </summary>
    public double PeriodMilliseconds { get; } = periodMilliseconds;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ZLinkStreamPacketAttribute : Attribute
{
}
