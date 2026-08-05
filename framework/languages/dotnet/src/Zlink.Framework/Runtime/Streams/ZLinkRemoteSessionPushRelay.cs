using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.Runtime.Streams;

// Cross-node bound-session push relay (spec 31 §6: the session route is
// framework-internal state). Core's bound-session send only reaches sessions
// whose STREAM service lives on the same MeshNode, so after a cross-node actor
// join or transfer an actor push must travel back to the session's node. The
// actor's node wraps the encoded stream frame in this internal node-addressed
// route packet; the session node writes it to the still-bound local session as
// raw stream bytes — the same bytes a local push produces.
internal static class ZLinkRemoteSessionPushProtocol
{
    public const string PacketName = "$zlink.session.push-relay.v1";
}

// Session-node → actor-node direction: a session frame whose bound actor
// migrated to another node is relayed to the actor's owner node and dispatched
// there through the standard actor inbound pipeline (the reply travels back on
// the bound-session push relay above).
internal static class ZLinkRemoteActorFrameProtocol
{
    public const string PacketName = "$zlink.actor.frame-relay.v1";
}

internal static class ZLinkRemoteActorReplyProtocol
{
    public const string PacketName = "$zlink.actor.reply-relay.v1";
}

// Relocation command 44/45. The target requests this only after the Actor
// authority CAS and completion work have succeeded. The session owner is the
// only writer of the bound-session route, so its ACK is the visibility fence.
internal static class ZLinkSessionRouteCommitProtocol
{
    public const string PacketName = "$zlink.session.route-commit.v1";
    public const string SealPacketName = "$zlink.session.route-seal.v1";
    public const string AbortPacketName = "$zlink.session.route-abort.v1";
    public const string UnsealPacketName = "$zlink.session.route-unseal.v1";
}

[ZLinkPacket(ZLinkSessionRouteCommitProtocol.SealPacketName)]
internal sealed record ZLinkSessionRouteSealRequest(
    string ActorId,
    string BindingToken,
    ulong BindingGeneration,
    ulong ObjectGeneration,
    ulong AuthorityOwnerGeneration,
    string MeshName,
    ulong TargetNodeGeneration,
    ulong OwnerLeaseGeneration,
    ulong SessionOwnerNodeGeneration,
    string HandoffId);

[ZLinkPacket(ZLinkSessionRouteCommitProtocol.AbortPacketName)]
internal sealed record ZLinkSessionRouteAbortRequest(
    string ActorId,
    string BindingToken,
    ulong BindingGeneration,
    ulong ObjectGeneration,
    ulong AuthorityOwnerGeneration,
    string MeshName,
    ulong TargetNodeGeneration,
    ulong OwnerLeaseGeneration,
    ulong SessionOwnerNodeGeneration,
    string HandoffId);

internal sealed record ZLinkSessionRouteSealReply(
    bool Acknowledged,
    ulong AcceptedHighWater);

[ZLinkPacket(ZLinkSessionRouteCommitProtocol.PacketName)]
internal sealed record ZLinkSessionRouteCommitRequest(
    string ActorId,
    string BindingToken,
    ulong BindingGeneration,
    ulong ObjectGeneration,
    ulong PreviousAuthorityOwnerGeneration,
    ulong TargetAuthorityOwnerGeneration,
    string PreviousMeshName,
    string TargetMeshName,
    ulong PreviousTargetNodeGeneration,
    ulong TargetNodeGeneration,
    ulong PreviousOwnerLeaseGeneration,
    ulong TargetOwnerLeaseGeneration,
    ulong SessionOwnerNodeGeneration,
    ulong AcceptedHighWater,
    string HandoffId,
    string TargetNodeRid);

[ZLinkPacket(ZLinkSessionRouteCommitProtocol.UnsealPacketName)]
internal sealed record ZLinkSessionRouteUnsealRequest(
    string ActorId,
    string BindingToken,
    ulong BindingGeneration,
    ulong ObjectGeneration,
    ulong PreviousAuthorityOwnerGeneration,
    ulong TargetAuthorityOwnerGeneration,
    string PreviousMeshName,
    string TargetMeshName,
    ulong PreviousTargetNodeGeneration,
    ulong TargetNodeGeneration,
    ulong PreviousOwnerLeaseGeneration,
    ulong TargetOwnerLeaseGeneration,
    ulong SessionOwnerNodeGeneration,
    ulong AcceptedHighWater,
    string HandoffId,
    string TargetNodeRid);

internal sealed record ZLinkSessionRouteCommitReply(
    bool Acknowledged,
    ulong AcceptedHighWater);

internal sealed class ZLinkSessionRouteSealHandler(ZLinkFrameworkRuntime runtime)
    : IZLinkRouteRequestHandler<
        ZLinkSessionRouteSealRequest,
        ZLinkSessionRouteSealReply>
{
    public async ValueTask<ZLinkSessionRouteSealReply> HandleAsync(
        ZLinkSessionRouteSealRequest message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var result = await runtime.SealSessionActorRouteAsync(
            new ZLinkSessionRouteSeal(
                message.ActorId,
                message.BindingToken,
                message.BindingGeneration,
                message.ObjectGeneration,
                message.AuthorityOwnerGeneration,
                message.MeshName,
                message.TargetNodeGeneration,
                message.OwnerLeaseGeneration,
                message.SessionOwnerNodeGeneration,
                message.HandoffId),
            cancellationToken).ConfigureAwait(false);
        //  Paired with route_control_sent on the requester: if this fires and
        //  the requester still times out, the loss is in the reply transport.
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"route_seal_replying actor={message.ActorId} ack={result.Acknowledged} "
            + $"source_node={context.SourceNodeRid}");
        return new ZLinkSessionRouteSealReply(
            result.Acknowledged,
            result.AcceptedHighWater);
    }
}

internal sealed class ZLinkSessionRouteAbortHandler(ZLinkFrameworkRuntime runtime)
    : IZLinkRouteRequestHandler<
        ZLinkSessionRouteAbortRequest,
        ZLinkSessionRouteSealReply>
{
    public ValueTask<ZLinkSessionRouteSealReply> HandleAsync(
        ZLinkSessionRouteAbortRequest message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        var request = new ZLinkSessionRouteSeal(
            message.ActorId,
            message.BindingToken,
            message.BindingGeneration,
            message.ObjectGeneration,
            message.AuthorityOwnerGeneration,
            message.MeshName,
            message.TargetNodeGeneration,
            message.OwnerLeaseGeneration,
            message.SessionOwnerNodeGeneration,
            message.HandoffId);
        var acknowledged = runtime.AbortSessionActorRouteSeal(request);
        return ValueTask.FromResult(
            new ZLinkSessionRouteSealReply(
                acknowledged,
                runtime.TryGetActorBoundSession(
                    message.ActorId,
                    out var session)
                    ? session.AcceptedHighWater
                    : 0));
    }
}

internal sealed class ZLinkSessionRouteCommitHandler(ZLinkFrameworkRuntime runtime)
    : IZLinkRouteRequestHandler<
        ZLinkSessionRouteCommitRequest,
        ZLinkSessionRouteCommitReply>
{
    public ValueTask<ZLinkSessionRouteCommitReply> HandleAsync(
        ZLinkSessionRouteCommitRequest message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        //  Paired with route_control_sent on the requester: the seal handler
        //  already traces its arrival, and without the same mark here a stalled
        //  commit cannot be told from one that never arrived.
        var result = CommitAndTrace(runtime, message);
        return ValueTask.FromResult(new ZLinkSessionRouteCommitReply(
            result.Acknowledged,
            result.AcceptedHighWater));
    }

    private static ZLinkSessionRouteCommitResult CommitAndTrace(
        ZLinkFrameworkRuntime runtime,
        ZLinkSessionRouteCommitRequest message)
    {
        var result = runtime.CommitSessionActorRoute(
            new ZLinkSessionRouteCommit(
                message.ActorId,
                message.BindingToken,
                message.BindingGeneration,
                message.ObjectGeneration,
                message.PreviousAuthorityOwnerGeneration,
                message.TargetAuthorityOwnerGeneration,
                message.PreviousMeshName,
                message.TargetMeshName,
                message.PreviousTargetNodeGeneration,
                message.TargetNodeGeneration,
                message.PreviousOwnerLeaseGeneration,
                message.TargetOwnerLeaseGeneration,
                message.SessionOwnerNodeGeneration,
                message.AcceptedHighWater,
                message.HandoffId,
                new ActorRef(
                    message.ActorId,
                    message.ObjectGeneration,
                    message.TargetMeshName,
                    RoutingId.FromHex(message.TargetNodeRid))));
        return result;
    }
}

internal sealed class ZLinkSessionRouteUnsealHandler(ZLinkFrameworkRuntime runtime)
    : IZLinkRouteRequestHandler<
        ZLinkSessionRouteUnsealRequest,
        ZLinkSessionRouteCommitReply>
{
    public ValueTask<ZLinkSessionRouteCommitReply> HandleAsync(
        ZLinkSessionRouteUnsealRequest message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        //  Paired with the commit handler's own arrival mark: without it a
        //  stalled unseal cannot be told from one that never arrived.
        var request = new ZLinkSessionRouteCommit(
            message.ActorId,
            message.BindingToken,
            message.BindingGeneration,
            message.ObjectGeneration,
            message.PreviousAuthorityOwnerGeneration,
            message.TargetAuthorityOwnerGeneration,
            message.PreviousMeshName,
            message.TargetMeshName,
            message.PreviousTargetNodeGeneration,
            message.TargetNodeGeneration,
            message.PreviousOwnerLeaseGeneration,
            message.TargetOwnerLeaseGeneration,
            message.SessionOwnerNodeGeneration,
            message.AcceptedHighWater,
            message.HandoffId,
            new ActorRef(
                message.ActorId,
                message.ObjectGeneration,
                message.TargetMeshName,
                RoutingId.FromHex(message.TargetNodeRid)));
        var acknowledged = runtime.UnsealCommittedSessionActorRoute(request);
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"route_unseal_result actor={message.ActorId} ack={acknowledged} "
            + $"accepted_high_water={message.AcceptedHighWater} handoff={message.HandoffId}");
        return ValueTask.FromResult(
            new ZLinkSessionRouteCommitReply(
                acknowledged,
                message.AcceptedHighWater));
    }
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

internal sealed class ZLinkRemoteActorFrameRelayHandler(ZLinkFrameworkRuntime runtime)
    : IZLinkRouteSendHandler<ZLinkRemoteActorFrameRelay>
{
    public async ValueTask HandleAsync(
        ZLinkRemoteActorFrameRelay message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        var requestSource = DecodeRequestSource(message);
        //  The only unlit hop on the relay: a frame that never gets here and one
        //  that is dropped after dispatch look identical from the sender.
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
                // A forwarded caller-routed frame carries no session identity.
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
        // A miss is a stale push racing a rebind or disconnect; spec 31 §6
        // forbids applying late pushes to a new binding, so it is dropped.
        // Backpressured writes retry within the request timeout.
        await runtime.DeliverRemoteSessionPushAsync(
                message,
                message.Frame,
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
