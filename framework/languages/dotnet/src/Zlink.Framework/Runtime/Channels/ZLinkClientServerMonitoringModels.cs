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
    Rejected = 5
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
    string? LastFailure);

internal sealed record ZLinkClientServerChannelSnapshot(
    string ChannelName,
    ZLinkClientServerRole LocalRole,
    bool Selectable,
    int ReadyServerCount,
    int ConnectionIntentCount,
    int PendingRequestCount,
    ulong Sequence,
    DateTimeOffset ObservedAt,
    IReadOnlyList<ZLinkClientServerServerSnapshot> Servers,
    ZLinkLocationRuntimeSnapshot Location);

internal sealed record ZLinkClientServerRuntimeEvent(
    string Identifier,
    ulong Sequence,
    DateTimeOffset Timestamp,
    string ChannelName,
    RoutingId? ServerRid,
    ulong? LifecycleGeneration,
    ulong? DescriptorRevision,
    int? Weight,
    bool? Ready,
    ZLinkClientServerServerState? State,
    string? Reason);
