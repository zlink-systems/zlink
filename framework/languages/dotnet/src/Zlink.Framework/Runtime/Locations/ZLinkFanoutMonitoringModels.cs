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
    ExcludedStale = 5,
}

internal sealed record ZLinkFanoutPublisherConnectionSnapshot(
    RoutingId PublisherRid,
    ulong LifecycleGeneration,
    ulong DescriptorRevision,
    string Endpoint,
    bool ConnectionIntent,
    bool Ready,
    ZLinkFanoutPublisherConnectionState State,
    string? LastFailure
);

internal sealed record ZLinkFanoutChannelSnapshot(
    string ChannelName,
    int ConnectionIntentCount,
    int ReadyConnectionCount,
    ulong Sequence,
    DateTimeOffset ObservedAt,
    IReadOnlyList<ZLinkFanoutPublisherConnectionSnapshot> Publishers,
    ZLinkLocationRuntimeSnapshot Location
);
