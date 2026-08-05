namespace Zlink.Framework.Runtime.Locations;

internal enum ZLinkRouteMeshTargetClassification
{
    Unknown = 0,
    ObjectClientTarget = 1,
    RequiredNotConnected = 2,
    ReadyEligible = 3
}

/// <summary>
/// Send-path view of the last auto-connect descriptor snapshot. It preserves
/// whether the target is an outbound-only Object Client so Node-direct
/// messaging does not confuse a deliberately unsupported target with a
/// temporarily disconnected eligible target. The runtime combines this
/// descriptor classification with its physical peer state without store I/O.
/// </summary>
internal interface IZLinkAutoConnectTopologyQuery
{
    ZLinkRouteMeshTargetClassification ClassifyRouteMeshTarget(
        string meshName,
        RoutingId nodeRid);

    IReadOnlyList<ZLinkRouteMeshPeerIdentity>? GetCompleteRouteMeshPeers(
        string meshName) => null;
}

internal readonly record struct ZLinkRouteMeshPeerIdentity(
    RoutingId NodeRid,
    ulong LifecycleGeneration,
    bool Draining);
