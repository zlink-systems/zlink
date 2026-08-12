using Zlink.Framework.Runtime.Backend.DotNet.Mappings;

namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkNativeActorJoinOperation(
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkRegistration registration,
    Func<string, ZLinkActorRuntimeState> getActorState)
{
    internal async ValueTask<ZLinkActorJoinResult> JoinAsync(
        IZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        IZLinkBackendSpotNode node,
        RoutingId targetNodeRid,
        string targetSpotId,
        string channelName,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        using var flow = ZLinkFlowContext.EnterCurrentOrCreate(
            ZLinkFlowOrigin.Application,
            runtime.Flow.CaptureEnabled);
        var encodedRequest = request.Encode(registration.Codecs);
        var joinHeader = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Command,
            channelName,
            typeof(ZLinkMessage).Name,
            encodedRequest.ContentType,
            null, null, null, null, null);
        var joinParts = ZLinkMessageParts.Create(
            ZLinkEnvelopeCodec.EncodeHeader(joinHeader),
            Message.From(encodedRequest.Payload.Bytes.Span));

        using var completion =
            new ZLinkNativeReplyCompletion<ZLinkBackendActorJoinResult>(
                cancellationToken);

        if (runtime.Flow.Enabled(ZLinkMessageFlowOutcome.Sent))
            runtime.Flow.Trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.Sent,
                ZLinkDispatchErrorSurface.SpotActor,
                ZLinkDispatchMessageKind.ActorRequest,
                "JoinSpot",
                channelName,
                SourceRid: targetNodeRid.ToString(),
                SpotId: targetSpotId,
                ActorId: actor.Context.ActorId));

        bool submitted;
        try
        {
            submitted = node.JoinActor(
                actorRef,
                targetNodeRid,
                targetSpotId,
                joinParts,
                completion.Complete,
                registration.DefaultRequestTimeout);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(joinParts);
        }

        if (!submitted)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor join submit failed for '{actor.Context.ActorId}' to SPOT '{targetSpotId}'.");

        var (joinResult, replyParts) = await completion.Task.ConfigureAwait(false);

        if (runtime.Flow.Enabled(ZLinkMessageFlowOutcome.ReplyReceived))
            runtime.Flow.Trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.ReplyReceived,
                ZLinkDispatchErrorSurface.SpotActor,
                ZLinkDispatchMessageKind.Response,
                "JoinSpot",
                channelName,
                SourceRid: targetNodeRid.ToString(),
                SpotId: targetSpotId,
                ActorId: actor.Context.ActorId));
        var reply = DecodeReply(
            joinResult.Result,
            replyParts,
            actor.Context.ActorId,
            targetSpotId);
        var accepted = joinResult.JoinResultCode == 0;
        var actorState = getActorState(actor.Context.ActorId);
        if (accepted)
        {
            actorState.BindNativeActorRef(joinResult.Actor);
            if (joinResult.Actor.NodeRid != actorRef.NodeRid)
                actorState.InvalidateContext();
        }

        return accepted
            ? new ZLinkActorJoinResult.Accepted(
                joinResult.Actor.ToNative(node.MeshStatus().MeshName),
                reply)
            : RejectedWithTrace(reply);
    }

    private ZLinkMessage DecodeReply(
        RequestResult result,
        IReadOnlyList<Message> replyParts,
        string actorId,
        string spotId)
    {
        try
        {
            if (result != RequestResult.Ok)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NotFound,
                    $"Actor join was rejected for '{actorId}' to SPOT '{spotId}'.");

            if (replyParts.Count == 0)
                throw new InvalidOperationException(
                    "Actor join reply was empty.");

            var header = ZLinkEnvelopeCodec.DecodeHeader(replyParts);
            var reply = (Message)ZLinkEnvelopeCodec.DecodeBody(
                replyParts,
                typeof(Message))!;
            using var ownedReply = Message.From(reply);
            return ZLinkMessage.FromEnvelopePayload(
                header.ContentType,
                ownedReply,
                registration.Codecs);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
    }

    private static ZLinkActorJoinResult.Rejected RejectedWithTrace(
        ZLinkMessage reply)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            "actor_join_rejected site=remote_joiner_tail");
        return new ZLinkActorJoinResult.Rejected(reply);
    }
}
