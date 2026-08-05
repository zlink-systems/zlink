using Systems.Zlink;

namespace Zlink.Framework.Contracts.Configuration;

public enum ZLinkTopologyState
{
    Starting = 0,
    Ready = 1,
    Degraded = 2,
    Stopping = 3,
    Stopped = 4,
    Failed = 5
}

public enum ZLinkTopologyReason
{
    RuntimeNotReady = 0,
    NoReadyPeer = 1,
    NoReadyTarget = 2,
    LocationUnavailable = 3,
    CapacityExceeded = 4,
    Draining = 5,
    InternalFailure = 6
}

public enum ZLinkPeerState
{
    Connecting = 0,
    Ready = 1,
    Draining = 2,
    NotConnected = 3,
    NotRequired = 4
}

public sealed record ZLinkChannelStatus(
    string ChannelName,
    bool IsReady,
    int ReadyTargetCount);

public sealed record ZLinkPeerStatus(
    RoutingId NodeRid,
    ZLinkPeerState State,
    ZLinkTopologyReason? UnavailableReason);

public sealed record ZLinkPlacementStatus(
    bool IsAvailable,
    int ActiveActorCount,
    int ActiveSpotCount,
    ZLinkTopologyReason? UnavailableReason);

public sealed record ZLinkRouteMeshStatus(
    string MeshName,
    ZLinkTopologyState State,
    bool IsReady,
    int ReadyPeerCount,
    IReadOnlyList<ZLinkChannelStatus> Channels,
    IReadOnlyList<ZLinkPeerStatus> Peers,
    ZLinkPlacementStatus Placement,
    ulong Sequence,
    DateTimeOffset ObservedAt);

public interface IZLinkRouteMeshRuntime
{
    ZLinkRouteMeshStatus GetStatus(string meshName);

    IAsyncEnumerable<ZLinkObservedStatus<ZLinkRouteMeshStatus>> ObserveAsync(
        string meshName,
        CancellationToken cancellationToken = default);
}

public enum ZLinkClientServerRole
{
    Client = 1,
    Server = 2,
    ClientAndServer = 3
}

public sealed record ZLinkClientServerTargetStatus(
    RoutingId NodeRid,
    int Weight,
    ZLinkPeerState State,
    ZLinkTopologyReason? UnavailableReason);

public sealed record ZLinkClientServerStatus(
    string ChannelName,
    ZLinkClientServerRole LocalRole,
    ZLinkTopologyState State,
    bool IsReady,
    int ReadyTargetCount,
    IReadOnlyList<ZLinkClientServerTargetStatus> Targets,
    ulong Sequence,
    DateTimeOffset ObservedAt);

public interface IZLinkClientServerRuntime
{
    ZLinkClientServerStatus GetStatus(string channelName);

    IAsyncEnumerable<ZLinkObservedStatus<ZLinkClientServerStatus>> ObserveAsync(
        string channelName,
        CancellationToken cancellationToken = default);
}
