namespace Zlink.Framework.Runtime.Actors;

internal static class ZLinkRemoteSessionBindingProtocol
{
    public const string PacketName = "framework.internal.actor-session-bind";
    public const string UnbindPacketName = "framework.internal.actor-session-unbind";
    public const string SessionOwnerTombstonePacketName =
        "framework.internal.actor-session-owner-tombstone";
}

// These records carry a binding projection across the wire. The Session owner
// binding table remains the authority for route replacement and accepted
// high-water state.
[ZLinkPacket(ZLinkRemoteSessionBindingProtocol.PacketName)]
internal sealed record ZLinkRemoteSessionBindRequest(
    string ActorId,
    byte[] TargetNodeRid,
    byte[] SessionNodeRid,
    byte[] SessionRid,
    string BindingToken,
    ulong BindingGeneration,
    ulong ObjectGeneration,
    string MeshName,
    ulong SessionOwnerNodeGeneration,
    ulong AcceptedHighWater,
    ZLinkRemoteSessionPreviousBinding? PreviousBinding = null);

internal sealed record ZLinkRemoteSessionPreviousBinding(
    byte[] TargetNodeRid,
    byte[] SessionNodeRid,
    byte[] SessionRid,
    string BindingToken,
    ulong BindingGeneration,
    ulong ObjectGeneration,
    string MeshName,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration,
    ulong SessionOwnerNodeGeneration,
    ulong AcceptedHighWater);

internal sealed record ZLinkRemoteSessionBindResponse(
    bool Acknowledged,
    ulong ObjectGeneration,
    string MeshName,
    byte[] TargetNodeRid,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration);

[ZLinkPacket(ZLinkRemoteSessionBindingProtocol.UnbindPacketName)]
internal sealed record ZLinkRemoteSessionUnbindRequest(
    string ActorId,
    byte[] TargetNodeRid,
    byte[] SessionNodeRid,
    byte[] SessionRid,
    string BindingToken,
    ulong BindingGeneration,
    ulong ObjectGeneration,
    string MeshName,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration,
    ulong SessionOwnerNodeGeneration,
    ulong AcceptedHighWater);

internal sealed record ZLinkRemoteSessionUnbindResponse(bool Acknowledged);

[ZLinkPacket(ZLinkRemoteSessionBindingProtocol.SessionOwnerTombstonePacketName)]
internal sealed record ZLinkRemoteSessionOwnerTombstoneRequest(
    string ActorId,
    byte[] ActorNodeRid,
    ulong ObjectGeneration,
    string MeshName,
    ulong ActorNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration,
    byte[] SessionRid,
    string BindingToken,
    ulong BindingGeneration,
    ulong SessionOwnerNodeGeneration);

internal sealed record ZLinkRemoteSessionOwnerTombstoneResponse(bool Acknowledged);

internal sealed class ZLinkRemoteSessionBindRouteHandler(
    ZLinkFrameworkRuntime runtime)
    : IZLinkRouteRequestHandler<
        ZLinkRemoteSessionBindRequest,
        ZLinkRemoteSessionBindResponse>
{
    public ValueTask<ZLinkRemoteSessionBindResponse> HandleAsync(
        ZLinkRemoteSessionBindRequest request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken) =>
        runtime.BindRemoteBoundSessionRouteAsync(
            request,
            context.SourceNodeRid,
            cancellationToken);
}

internal sealed class ZLinkRemoteSessionUnbindRouteHandler(
    ZLinkFrameworkRuntime runtime)
    : IZLinkRouteRequestHandler<
        ZLinkRemoteSessionUnbindRequest,
        ZLinkRemoteSessionUnbindResponse>
{
    public ValueTask<ZLinkRemoteSessionUnbindResponse> HandleAsync(
        ZLinkRemoteSessionUnbindRequest request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        return runtime.TombstoneRemoteActorBindingAsync(
            request,
            context.SourceNodeRid,
            cancellationToken);
    }
}

internal sealed class ZLinkRemoteSessionOwnerTombstoneRouteHandler(
    ZLinkFrameworkRuntime runtime)
    : IZLinkRouteRequestHandler<
        ZLinkRemoteSessionOwnerTombstoneRequest,
        ZLinkRemoteSessionOwnerTombstoneResponse>
{
    public ValueTask<ZLinkRemoteSessionOwnerTombstoneResponse> HandleAsync(
        ZLinkRemoteSessionOwnerTombstoneRequest request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        return runtime.TombstoneRemoteSessionOwnerBindingAsync(
            request,
            context.SourceNodeRid,
            cancellationToken);
    }
}
