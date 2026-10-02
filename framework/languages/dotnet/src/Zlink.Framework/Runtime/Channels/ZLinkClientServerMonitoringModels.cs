using Systems.Zlink;
using Zlink.Framework.Contracts.Locations;

namespace Zlink.Framework.Contracts.Configuration;

internal enum ZLinkClientServerServerState
{
    Configured = 0,
    Connecting = 1,
    Ready = 2,
    Draining = 3,
    Disconnected = 4,
    Rejected = 5,
}

internal sealed record ZLinkClientServerServerSnapshot(
    RoutingId ServerRid,
    ulong LifecycleGeneration,
    ulong DescriptorRevision,
    string Endpoint,
    int Weight,
    bool Ready,
    ZLinkClientServerServerState State,
    string DescriptorSource,
    string? LastFailure
);

internal sealed record ZLinkClientServerChannelSnapshot(
    string ChannelName,
    ZLinkClientServerRole LocalRole,
    bool IsReady,
    int ReadyServerCount,
    int ConnectionIntentCount,
    int PendingRequestCount,
    ulong Sequence,
    DateTimeOffset ObservedAt,
    IReadOnlyList<ZLinkClientServerServerSnapshot> Servers,
    ZLinkLocationRuntimeSnapshot Location
);
