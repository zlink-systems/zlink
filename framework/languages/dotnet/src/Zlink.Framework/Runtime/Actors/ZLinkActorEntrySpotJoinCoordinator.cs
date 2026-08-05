using Systems.Zlink.Stream.Connector.Runtime;

namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorEntrySpotJoinCoordinator(
    ZLinkFrameworkRegistration registration,
    ZLinkSpotRuntimeManager spots,
    ZLinkActorSessionManager actorSessionManager,
    Func<ZLinkFrameworkComponentState> getState,
    Func<IZLinkBackendSpotNode?> getActorSpotNode,
    ZLinkMessageFlowTracer flow)
{
    public async ValueTask<ZLinkActorJoinResult> JoinAsync(
        RoutingId spotNodeRid,
        IZLinkActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var actorState = actorSessionManager.GetOrCreateState(actor.Context.ActorId);
        var node = getActorSpotNode()
                   ?? throw new InvalidOperationException("Entry SPOT join requires a router-capable SpotNode.");
        var actorRef = actorState.NativeActorRef
                       ?? throw new InvalidOperationException(
                           $"Actor '{actor.Context.ActorId}' does not have a native Actor ref.");
        var previousActivation = actorState.LiveActivation;

        // A target registered in this process has a managed Entry Spot
        // activation that can perform admission and lifecycle updates without
        // entering the native routed-operation path. Keep native JoinEntrySpot
        // for targets that are genuinely outside this process; otherwise a
        // local operation can wait for a route callback that this process does
        // not produce.
        if (getState().TryGetSpotNodeByRoutingId(spotNodeRid, out var localTarget))
        {
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"entry_join_local_managed actor={actor.Context.ActorId} "
                + $"target_node={spotNodeRid}");
            return await JoinLocalEntrySpotAsync(
                    localTarget,
                    actor,
                    actorState,
                    actorRef,
                    previousActivation,
                    request,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        using var completion = new ZLinkNativeReplyCompletion<ZLinkBackendActorJoinEntrySpotResult>(
            cancellationToken);

        var correlationId = ZlinkStreamCorrelation.Next();
        if (flow.Enabled(ZLinkMessageFlowOutcome.Sent))
            flow.Trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.Sent,
                ZLinkDispatchErrorSurface.SpotActor,
                ZLinkDispatchMessageKind.ActorRequest,
                "JoinEntrySpot",
                CorrelationId: correlationId,
                SourceRid: spotNodeRid.ToString(),
                ActorId: actor.Context.ActorId));

        var encodedRequest = request.Encode(registration.Codecs);
        using (var nativeRequest = ZLinkEnvelopeCodec.EncodePart(new ZLinkActorJoinSinglePartEnvelope(
                   encodedRequest.ContentType,
                   encodedRequest.Payload.ToArray())))
        {
            if (!node.JoinActorEntrySpot(
                    actorRef,
                    spotNodeRid,
                    nativeRequest,
                    completion.Complete,
                    registration.DefaultRequestTimeout))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NotFound,
                    $"Actor entry SPOT join submit failed for '{actor.Context.ActorId}'.");
        }

        var (result, replyParts) = await completion.Task.ConfigureAwait(false);
        if (result.Result == RequestResult.NotConnected)
        {
            ZLinkMessageParts.DisposeAll(replyParts);
            // Not connected locally — the remote fallback below is traced by the
            // route client; no reply_received here.
            return await JoinRemoteAsync(
                    getState(),
                    spotNodeRid,
                    actor,
                    actorState,
                    actorRef,
                    previousActivation,
                    request,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        if (flow.Enabled(ZLinkMessageFlowOutcome.ReplyReceived))
            flow.Trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.ReplyReceived,
                ZLinkDispatchErrorSurface.SpotActor,
                ZLinkDispatchMessageKind.Response,
                "JoinEntrySpot",
                CorrelationId: correlationId,
                SourceRid: spotNodeRid.ToString(),
                ActorId: actor.Context.ActorId));

        var reply = DecodeEntrySpotJoinReply(result.Result, replyParts, actor.Context.ActorId, spotNodeRid);
        var accepted = result.JoinResultCode == 0;
        if (result.Result != RequestResult.Ok)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor entry SPOT join failed for '{actor.Context.ActorId}' with '{result.Result}'.");

        if (accepted)
        {
            actorState.BindNativeActorRef(result.Actor);
            await NotifyManagedEntrySpotJoinLifecycleAsync(
                    actor,
                    previousActivation,
                    result.Actor.NodeRid,
                    cancellationToken)
                .ConfigureAwait(false);
            if (result.Actor.NodeRid != actorRef.NodeRid)
            {
                actorState.InvalidateContext();
                // Native entry-spot join: no framework runtime claims the
                // row on the target, so this owner renews it with the new
                // node rid instead of releasing it.
                await actorSessionManager.RenewActorLocationAfterEntrySpotMoveAsync(
                        actorState,
                        result.Actor.NodeRid,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
        }

        return accepted
            ? new ZLinkActorJoinResult.Accepted(
                result.Actor.ToNative(node.MeshStatus().MeshName),
                reply)
            : new ZLinkActorJoinResult.Rejected(reply);
    }

    private async ValueTask<ZLinkActorJoinResult> JoinRemoteAsync(
        ZLinkFrameworkComponentState state,
        RoutingId spotNodeRid,
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkSpotActivation? previousActivation,
        ZLinkMessage joinRequest,
        CancellationToken cancellationToken)
    {
        if (state.TryGetSpotNodeByRoutingId(spotNodeRid, out var targetNode))
            return await JoinLocalEntrySpotAsync(
                    targetNode,
                    actor,
                    actorState,
                    sourceActorRef,
                    previousActivation,
                    joinRequest,
                    cancellationToken)
                .ConfigureAwait(false);

        var sourceNode = getActorSpotNode();
        var nodeRuntime = state.SpotNodes.Values.FirstOrDefault(
                              candidate => ReferenceEquals(candidate.Node, sourceNode))
                          ?? state.SpotNodes.Values.FirstOrDefault(
                              candidate => candidate.Registration.Router is not null)
                          ?? throw new ZLinkFrameworkException(
                              ZLinkFrameworkErrorKind.NotFound,
                              $"Actor entry SPOT join failed for '{actor.Context.ActorId}' because no router-capable MeshNode is registered.");

        var request = ZLinkActorEntrySpotRoutePackets.CreateJoinRequest(
            actor.Context.ActorId,
            actorState.ActorType ?? actor.GetType().Name,
            sourceActorRef,
            previousActivation?.SpotId,
            joinRequest,
            registration.Codecs);

        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request,
            nodeRuntime.Name,
            ZLinkActorEntrySpotRoutePackets.JoinEntrySpotPacketName,
            registration.DefaultRequestTimeout);
        var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
            header,
            request,
            registration.Codecs);
        var replyParts = await nodeRuntime
            .RequestToNodeAsync(
                spotNodeRid,
                parts,
                registration.DefaultRequestTimeout,
                cancellationToken)
            .ConfigureAwait(false);
        var reply = ZLinkClientCallCodec
            .DecodeEnvelopeReplyAndDispose<ZLinkActorEntrySpotRouteJoinReply>(
                replyParts,
                "Actor EntrySpot join reply is empty.",
                $"Actor EntrySpot join failed for '{actor.Context.ActorId}'.",
                registration.Codecs);

        var replyMessage = ZLinkActorEntrySpotRoutePackets.DecodeJoinReplyPayload(
            reply,
            registration.Codecs);
        if (!reply.Accepted)
        {
            //  Application 정책이 거절한 것인지, framework가 다른 이유로 만든
            //  Accepted=false인지 밖에서 구분할 수 없다.
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"entry_join_rejected path=remote actor={actor.Context.ActorId}");
            return new ZLinkActorJoinResult.Rejected(replyMessage);
        }

        var targetRef = ZLinkActorEntrySpotRoutePackets.ToActorRef(reply);
        actorState.BindNativeActorRef(targetRef);
        if (previousActivation is not null)
            await previousActivation.NotifyActorLeftAfterNativeJoinEntrySpotAsync(
                    actor,
                    cancellationToken)
                .ConfigureAwait(false);
        if (targetRef.NodeRid != sourceActorRef.NodeRid)
        {
            actorState.InvalidateContext();
            // Routed entry-spot join: the target runtime creates the actor
            // through its own claim (Takeover); this owner releases.
            await actorSessionManager.ReleaseActorLocationAfterMoveAsync(actorState, cancellationToken)
                .ConfigureAwait(false);
        }

        return new ZLinkActorJoinResult.Accepted(
            targetRef.ToNative(nodeRuntime.Node.MeshStatus().MeshName),
            replyMessage);
    }

    private async ValueTask<ZLinkActorJoinResult> JoinLocalEntrySpotAsync(
        ZLinkSpotNodeRuntime targetNode,
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkSpotActivation? previousActivation,
        ZLinkMessage joinRequest,
        CancellationToken cancellationToken)
    {
        var activation = targetNode.EntrySpotActivation
                         ?? throw new ZLinkFrameworkException(
                             ZLinkFrameworkErrorKind.NotFound,
                             $"Actor entry SPOT join target node '{targetNode.Node.RoutingId}' does not have an Entry Spot activation.");
        var localTargetRef = targetNode.Node.ActorLookup(actor.Context.ActorId);
        var createdHere = localTargetRef is null;
        ZLinkBackendActorRef targetRef;
        if (localTargetRef is { } existing)
        {
            targetRef = existing;
        }
        else
        {
            using var emptyCreateRequest = Message.From(ReadOnlySpan<byte>.Empty);
            targetRef = targetNode.Node.CreateActor(actor.Context.ActorId, emptyCreateRequest);
        }

        ZLinkSpotActorJoinResult admission;
        try
        {
            // Entry Spot admission does not require an OnActorJoin callback.
            // The lifecycle callback remains available for user-defined work,
            // while the standard Entry Spot join contract accepts the actor.
            admission = ZLinkSpotActorJoinResult.Accept();
        }
        catch (Exception admissionFailure)
        {
            if (createdHere)
                try
                {
                    await actorSessionManager.CompensateUncommittedNativeActorAsync(
                            targetNode.Node,
                            targetRef,
                            "local-entry-spot-admission")
                        .ConfigureAwait(false);
                }
                catch (Exception cleanupFailure)
                {
                    throw new AggregateException(admissionFailure, cleanupFailure);
                }
            throw;
        }

        var reply = CopyReply(admission.Reply);
        if (!admission.Accepted)
        {
            if (createdHere)
                await actorSessionManager.CompensateUncommittedNativeActorAsync(
                        targetNode.Node,
                        targetRef,
                        "local-entry-spot-rejection")
                    .ConfigureAwait(false);
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"entry_join_rejected path=local actor={actorState.ActorId} "
                + $"created_here={createdHere}");
            return new ZLinkActorJoinResult.Rejected(reply);
        }

        actorState.BindNativeActorRef(targetRef);
        await NotifyManagedEntrySpotJoinLifecycleAsync(
                actor,
                previousActivation,
                targetRef.NodeRid,
                cancellationToken)
            .ConfigureAwait(false);
        if (targetRef.NodeRid != sourceActorRef.NodeRid)
        {
            actorState.InvalidateContext();
            // Local cross-node entry-spot move within this process keeps
            // the same owner; renew the row with the new node rid.
            await actorSessionManager.RenewActorLocationAfterEntrySpotMoveAsync(
                    actorState,
                    targetRef.NodeRid,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        return new ZLinkActorJoinResult.Accepted(
            targetRef.ToNative(targetNode.Node.MeshStatus().MeshName),
            reply);
    }

    private async ValueTask NotifyManagedEntrySpotJoinLifecycleAsync(
        IZLinkActor actor,
        ZLinkSpotActivation? previousActivation,
        RoutingId targetNodeRid,
        CancellationToken cancellationToken)
    {
        await NotifyManagedUserSpotLeftForEntrySpotJoinAsync(
                actor,
                previousActivation,
                cancellationToken)
            .ConfigureAwait(false);

        await spots.EntrySpotActors.NotifyJoinedAsync(
                getState(),
                actor,
                targetNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static async ValueTask NotifyManagedUserSpotLeftForEntrySpotJoinAsync(
        IZLinkActor actor,
        ZLinkSpotActivation? previousActivation,
        CancellationToken cancellationToken)
    {
        if (previousActivation is null) return;

        await previousActivation.NotifyActorLeftAfterNativeJoinEntrySpotAsync(
                actor,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static ZLinkMessage CopyReply(ZLinkMessage? reply)
    {
        return reply ?? ZLinkMessage.Empty;
    }

    private ZLinkMessage DecodeEntrySpotJoinReply(
        RequestResult result,
        IReadOnlyList<Message> replyParts,
        string actorId,
        RoutingId spotNodeRid)
    {
        try
        {
            if (result != RequestResult.Ok)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NotFound,
                    $"Actor entry SPOT join was rejected for '{actorId}' to node '{spotNodeRid}'.");

            if (replyParts.Count == 0) return ZLinkMessage.Empty;

            if (replyParts.Count == 1)
                return ZLinkMessage.FromEnvelopePayload(
                    ZLinkEnvelopeCodec.DefaultContentType,
                    replyParts[0],
                    registration.Codecs);

            var header = ZLinkEnvelopeCodec.DecodeHeader(replyParts);
            return ZLinkMessage.FromEnvelopePayload(header.ContentType, replyParts[1], registration.Codecs);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
    }
}
