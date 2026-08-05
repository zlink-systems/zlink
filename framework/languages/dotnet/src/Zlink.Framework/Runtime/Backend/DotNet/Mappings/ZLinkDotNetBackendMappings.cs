namespace Zlink.Framework.Runtime.Backend.DotNet.Mappings;

internal static class ZLinkDotNetBackendMappings
{
    public static ZLinkSpotNodeStatus ToFramework(this MeshNodeStatus status)
    {
        return new ZLinkSpotNodeStatus(
            status.MeshName,
            status.LocalEndpoint,
            status.RoutingId,
            MapNodeState(status.State),
            status.ConfiguredPeerCount,
            status.AdmittedPeerCount,
            status.AdmittedPeerCount,
            status.ChannelCount,
            status.ChannelCount,
            status.LastError,
            status.LastChangedMs);
    }

    private static ZLinkSpotNodeState MapNodeState(MeshNodeState state)
    {
        return state switch
        {
            MeshNodeState.Created => ZLinkSpotNodeState.Idle,
            MeshNodeState.Started => ZLinkSpotNodeState.Connecting,
            MeshNodeState.PartialReady => ZLinkSpotNodeState.PartialReady,
            MeshNodeState.Ready => ZLinkSpotNodeState.Ready,
            MeshNodeState.Error => ZLinkSpotNodeState.Error,
            _ => ZLinkSpotNodeState.Idle
        };
    }

    public static ZLinkSpotNodePeerEntry ToFramework(
        this MeshNodePeer peer,
        string localEndpoint,
        MeshPeerChannel? channel)
    {
        return new ZLinkSpotNodePeerEntry(
            channel?.Name ?? string.Empty,
            localEndpoint,
            peer.Endpoint,
            MapPeerSource(peer.Source),
            ZLinkSpotPeerKind.SpotMesh,
            MapPeerState(peer.State),
            channel is null ? 0 : checked((int)channel.Weight),
            0,
            peer.LastChangedMs);
    }

    private static ZLinkSpotPeerSource MapPeerSource(MeshPeerSource source)
    {
        return source switch
        {
            MeshPeerSource.Manual => ZLinkSpotPeerSource.Manual,
            MeshPeerSource.Discovery => ZLinkSpotPeerSource.Discovery,
            _ => ZLinkSpotPeerSource.Mixed
        };
    }

    private static ZLinkSpotPeerState MapPeerState(MeshPeerState state)
    {
        return state switch
        {
            MeshPeerState.Configured => ZLinkSpotPeerState.Configured,
            MeshPeerState.Connecting => ZLinkSpotPeerState.Connecting,
            MeshPeerState.Admitted => ZLinkSpotPeerState.Connected,
            _ => ZLinkSpotPeerState.Configured
        };
    }

    public static ZLinkBackendSocketMonitorEvent ToFramework(this MonitorEvent monitorEvent)
    {
        return new ZLinkBackendSocketMonitorEvent(
            (ZLinkSocketNativeEventType)monitorEvent.Event,
            monitorEvent.RoutingId,
            monitorEvent.LocalAddr,
            monitorEvent.RemoteAddr,
            monitorEvent.Value);
    }

    public static ZLinkBackendActorRef ToBackend(this ActorRef actorRef)
    {
        return new ZLinkBackendActorRef(
            actorRef.NodeRid,
            actorRef.ActorId,
            actorRef.ObjectGeneration);
    }

    public static ActorRef ToNative(
        this ZLinkBackendActorRef actorRef,
        string meshName)
    {
        return new ActorRef(
            actorRef.ActorId,
            actorRef.Generation,
            meshName,
            actorRef.NodeRid);
    }
}
