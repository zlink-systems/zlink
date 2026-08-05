using Systems.Zlink;
using Zlink.Framework.Contracts.Locations;

namespace Zlink.Framework.Contracts.Configuration;

internal enum ZLinkFanoutPublisherConnectionState
{
    Connecting = 0,
    Ready = 1,
    Disconnected = 2,
    Reconnecting = 3,
    ExcludedDraining = 4,
    ExcludedStale = 5
}

internal sealed record ZLinkFanoutPublisherConnectionSnapshot(
    RoutingId PublisherRid,
    ulong LifecycleGeneration,
    ulong DescriptorRevision,
    string Endpoint,
    bool ConnectionIntent,
    bool Ready,
    ZLinkFanoutPublisherConnectionState State,
    string? LastFailure);

internal sealed record ZLinkFanoutChannelSnapshot(
    string ChannelName,
    int ConnectionIntentCount,
    int ReadyConnectionCount,
    ulong Sequence,
    DateTimeOffset ObservedAt,
    IReadOnlyList<ZLinkFanoutPublisherConnectionSnapshot> Publishers,
    ZLinkLocationRuntimeSnapshot Location);

internal abstract record ZLinkFanoutRuntimeEvent
{
    private protected ZLinkFanoutRuntimeEvent(
        string identifier,
        ulong sequence,
        DateTimeOffset timestamp,
        string channelName)
    {
        Identifier = identifier;
        Sequence = sequence;
        Timestamp = timestamp;
        ChannelName = channelName;
    }

    internal string Identifier { get; }
    internal ulong Sequence { get; }
    internal DateTimeOffset Timestamp { get; }
    internal string ChannelName { get; }

    internal sealed record PublisherChanged(
        ulong Sequence,
        DateTimeOffset Timestamp,
        string ChannelName,
        ZLinkFanoutPublisherConnectionSnapshot Entry)
        : ZLinkFanoutRuntimeEvent(
            "zlink.runtime.fanout.publisher_changed",
            Sequence,
            Timestamp,
            ChannelName);

    internal sealed record LocationChanged(
        ulong Sequence,
        DateTimeOffset Timestamp,
        string ChannelName,
        ZLinkLocationRuntimeSnapshot Location)
        : ZLinkFanoutRuntimeEvent(
            "zlink.runtime.location.store_changed",
            Sequence,
            Timestamp,
            ChannelName);

    internal sealed record RuntimeChanged(
        ulong Sequence,
        DateTimeOffset Timestamp,
        string ChannelName)
        : ZLinkFanoutRuntimeEvent(
            "zlink.runtime.framework.state_changed",
            Sequence,
            Timestamp,
            ChannelName);
}
