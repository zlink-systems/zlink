using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.Runtime.Streams;

// Cross-node bound-session push relay. Core's bound-session send only reaches
// sessions whose STREAM service is on the same MeshNode, so the Actor node
// relays the encoded stream frame to the Session node.
internal static class ZLinkRemoteSessionPushProtocol
{
    public const string PacketName = "$zlink.session.push-relay.v1";
}

internal static class ZLinkRemoteActorFrameProtocol
{
    public const string PacketName = "$zlink.actor.frame-relay.v1";
}

internal static class ZLinkRemoteActorReplyProtocol
{
    public const string PacketName = "$zlink.actor.reply-relay.v1";
}

internal static class ZLinkRemoteActorSourceLeaveProtocol
{
    public const string PacketName = "$zlink.actor.source-leave.v1";
}

internal sealed record ZLinkRemoteActorFrameRelay(
    string ActorId,
    ulong ActorGeneration,
    string TargetNodeRid,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration,
    string RelayNodeRid,
    ulong RelayNodeGeneration,
    string SourceNodeRid,
    ulong SourceNodeGeneration,
    string SourceSessionRid,
    string? RequestSourceOwnerId,
    ulong RequestSourceLeaseGeneration,
    string? RequestSourceNodeRid,
    ulong RequestSourceNodeGeneration,
    ulong OperationIdHigh,
    ulong OperationIdLow,
    byte MessageFollowHopCount,
    ulong ReplyRequestId,
    uint ReplyFlags,
    string? ReplyCapability,
    ulong DeadlineUnixMs,
    byte[] ApplicationMetadata,
    byte[] Header,
    byte[] Body);

internal sealed record ZLinkRemoteActorReplyRelay(
    string ActorId,
    ulong RequestId,
    uint Flags,
    string ReplyCapability,
    string ResponderNodeRid,
    byte[] Frame);

internal sealed record ZLinkRemoteActorSourceLeave(
    string ActorId,
    string HandoffId,
    ulong ActorGeneration,
    string TargetNodeRid,
    ulong TargetAuthorityOwnerGeneration);

internal sealed class ZLinkRemoteActorFrameRelayHandler(ZLinkFrameworkRuntime runtime)
    : IZLinkRouteSendHandler<ZLinkRemoteActorFrameRelay>
{
    public async ValueTask HandleAsync(
        ZLinkRemoteActorFrameRelay message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        var requestSource = DecodeRequestSource(message);
        await runtime.DispatchRemoteActorFrameAsync(
                message.ActorId,
                message.ActorGeneration,
                RoutingId.FromHex(message.TargetNodeRid),
                message.TargetNodeGeneration,
                message.AuthorityOwnerGeneration,
                message.OwnerLeaseGeneration,
                context.SourceNodeRid,
                RoutingId.FromHex(message.RelayNodeRid),
                message.RelayNodeGeneration,
                message.SourceNodeRid is { Length: > 0 } nodeHex
                    ? RoutingId.FromHex(nodeHex)
                    : default,
                message.SourceNodeGeneration,
                message.SourceSessionRid is { Length: > 0 } sessionHex
                    ? RoutingId.FromHex(sessionHex)
                    : default,
                requestSource,
                message.ApplicationMetadata,
                new MeshOperationId(
                    message.OperationIdHigh,
                    message.OperationIdLow),
                message.MessageFollowHopCount,
                message.ReplyRequestId,
                message.ReplyFlags,
                message.ReplyCapability,
                message.DeadlineUnixMs,
                message.Header,
                message.Body,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static ZLinkServiceWireCodec.RequestSourceFence? DecodeRequestSource(
        ZLinkRemoteActorFrameRelay message)
    {
        var hasOwner = !string.IsNullOrWhiteSpace(message.RequestSourceOwnerId);
        var hasNode = !string.IsNullOrWhiteSpace(message.RequestSourceNodeRid);
        var hasFence = hasOwner
                       || message.RequestSourceLeaseGeneration != 0
                       || hasNode
                       || message.RequestSourceNodeGeneration != 0;
        if (!hasFence) return null;
        if (!hasOwner || message.RequestSourceLeaseGeneration == 0
            || !hasNode || message.RequestSourceNodeGeneration == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{message.ActorId}' relay request-source fence is incomplete.");
        return new ZLinkServiceWireCodec.RequestSourceFence(
            message.RequestSourceOwnerId!,
            message.RequestSourceLeaseGeneration,
            RoutingId.FromHex(message.RequestSourceNodeRid!),
            message.RequestSourceNodeGeneration);
    }
}

internal sealed record ZLinkRemoteSessionPushRelay(
    string ActorId,
    ulong ObjectGeneration,
    string MeshName,
    string TargetNodeRid,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration,
    string BindingToken,
    ulong BindingGeneration,
    ulong SessionOwnerNodeGeneration,
    string SessionRid,
    byte[] Frame);

internal sealed class ZLinkRemoteSessionPushRelayHandler(ZLinkFrameworkRuntime runtime)
    : IZLinkRouteSendHandler<ZLinkRemoteSessionPushRelay>
{
    public async ValueTask HandleAsync(
        ZLinkRemoteSessionPushRelay message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        await runtime.AdmitRemoteSessionPushOneWayAsync(
                message,
                context.SourceNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
    }
}

internal sealed class ZLinkRemoteActorReplyRelayHandler(ZLinkFrameworkRuntime runtime)
    : IZLinkRouteSendHandler<ZLinkRemoteActorReplyRelay>
{
    public async ValueTask HandleAsync(
        ZLinkRemoteActorReplyRelay message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"remote_actor_reply_handler actor={message.ActorId} request_id={message.RequestId} "
            + $"source_node={context.SourceNodeRid} responder_node={message.ResponderNodeRid}");
        await runtime.DeliverRemoteActorReplyAsync(
                message.ActorId,
                message.RequestId,
                message.Flags,
                message.ReplyCapability,
                context.SourceNodeRid,
                RoutingId.FromHex(message.ResponderNodeRid),
                message.Frame,
                cancellationToken)
            .ConfigureAwait(false);
    }
}

internal sealed class ZLinkRemoteActorSourceLeaveHandler(ZLinkFrameworkRuntime runtime)
    : IZLinkRouteSendHandler<ZLinkRemoteActorSourceLeave>
{
    public ValueTask HandleAsync(
        ZLinkRemoteActorSourceLeave message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var authenticatedTargetNodeRid = context.SourceNodeRid;
        runtime.RunDetached(
            $"actor-source-leave:{message.ActorId}:{message.HandoffId}",
            token => runtime.DeliverRemoteActorSourceLeaveAsync(
                message,
                authenticatedTargetNodeRid,
                token));
        return ValueTask.CompletedTask;
    }
}
