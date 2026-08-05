using Systems.Zlink;
using Zlink.Framework.Contracts.Locations;

namespace Zlink.Framework.Contracts.Configuration;

internal enum ZLinkMeshNodeState
{
    Starting = 0,
    Serving = 1,
    Draining = 2,
    Drained = 3,
    ForceStopping = 4,
    Stopped = 5,
    Faulted = 6
}

internal sealed record ZLinkMeshPeerSnapshot(
    RoutingId Rid,
    ulong LifecycleGeneration,
    ulong DescriptorRevision,
    string Endpoint,
    string AdmissionState,
    bool Ready,
    string DrainState,
    IReadOnlyList<string> ChannelNames,
    string? LastFailure);

internal sealed record ZLinkMeshChannelSnapshot(
    string ChannelName,
    int LocalWeight,
    int ReadyMemberCount,
    bool Selectable);

internal sealed record ZLinkMeshClaimSnapshot(
    bool ApplicationActive,
    ulong PendingApplicationWork,
    bool InfrastructureActive,
    ulong PendingInfrastructureWork);

internal sealed record ZLinkLocationRuntimeSnapshot(
    string State,
    DateTimeOffset? LastSuccessAt,
    DateTimeOffset? LastFailureAt);

internal sealed record ZLinkInstanceSpotTypeSnapshot(
    string InstanceSpotType,
    ulong ActiveCount,
    ulong ActivatingCount,
    ulong ClosingCount,
    ulong PendingMessageCount,
    ulong PendingByteCount,
    string? LastActivationOutcome);

internal sealed record ZLinkMeshNodeSnapshot(
    string MeshName,
    RoutingId Rid,
    ulong LifecycleGeneration,
    ulong DescriptorRevision,
    string Endpoint,
    ZLinkMeshNodeState State,
    ulong Sequence,
    DateTimeOffset ObservedAt,
    IReadOnlyList<string> DescriptorSources,
    IReadOnlyList<ZLinkMeshPeerSnapshot> Peers,
    IReadOnlyList<ZLinkMeshChannelSnapshot> Channels,
    ZLinkMeshClaimSnapshot Claims,
    ZLinkLocationRuntimeSnapshot Location)
{
    internal long ApplicationVersion { get; init; }
    internal ZLinkMeshNodeObjectRole ObjectRole { get; init; }
    internal int PlacementWeight { get; init; } = 100;
    internal ZLinkPlacementCapacity PopulationCapacity { get; init; }
        = new(
            new ZLinkPopulationCapacity(0, 0, 0),
            new ZLinkPopulationCapacity(0, 0, 0),
            Array.Empty<ZLinkSpotTypeCapacity>());
    internal ZLinkActivationConcurrency ActivationConcurrency { get; init; }
        = new(0, 128);
    internal ulong PlacementReservationFailureCount { get; init; }
    internal string? LastPlacementReservationFailure { get; init; }
    internal IReadOnlyList<ZLinkObjectCapability> ObjectCapabilities { get; init; }
        = Array.Empty<ZLinkObjectCapability>();
    internal IReadOnlyList<ZLinkInstanceSpotTypeSnapshot> InstanceSpots { get; init; }
        = Array.Empty<ZLinkInstanceSpotTypeSnapshot>();
}

internal sealed record ZLinkMeshRuntimeEvent(
    string Identifier,
    ulong Sequence,
    DateTimeOffset Timestamp,
    string MeshName,
    RoutingId SourceRid,
    RoutingId? PeerRid,
    ulong? LifecycleGeneration,
    ulong? DescriptorRevision,
    string? ChannelName,
    string? ClaimDomain,
    string? MessageKind,
    string? PlacementOutcome,
    ZLinkCapacityVector? Capacity,
    ZLinkPlacementCapacity? PopulationCapacity,
    ZLinkActivationConcurrency? ActivationConcurrency,
    string? Reason,
    ZLinkMeshNodeState? State);
