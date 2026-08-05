namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkActorMessageFollowDispatcher
{
    internal static bool CanFollow(
        ZLinkActorRuntimeState actorState,
        ZLinkBackendActorRef frameActor,
        ZLinkBackendActorRouteContext routeContext)
    {
        var route = actorState.Handoff.RouteFrame(
            actorState.NativeActorRef,
            frameActor,
            out var messageFollowRoute);
        if (route != ZLinkActorFrameRoute.MessageFollow)
            return false;
        try
        {
            ValidateMessageFollowRoute(messageFollowRoute!.Value, routeContext);
            return true;
        }
        catch (ZLinkFrameworkException exception)
            when (exception.Kind == ZLinkFrameworkErrorKind.Unavailable)
        {
            return false;
        }
    }

    public static bool TryFollow(
        ZLinkFrameworkRuntime runtime,
        ZLinkActorRuntimeState actorState,
        ZLinkBackendActorRef frameActor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags,
        ZLinkBackendActorRouteContext routeContext,
        ZlinkStreamHeader header,
        Message body,
        ulong sourceNodeGeneration = 0,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource = null,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? directReply = null,
        ReadOnlyMemory<byte> applicationMetadata = default)
    {
        var route = actorState.Handoff.RouteFrame(
            actorState.NativeActorRef,
            frameActor,
            out var messageFollowRoute);
        if (route is not ZLinkActorFrameRoute.Current)
        {
            var currentActor = actorState.NativeActorRef;
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_frame_route actor={frameActor.ActorId} route={route} "
                + $"frame_node={frameActor.NodeRid} frame_generation={frameActor.Generation} "
                + $"current_node={currentActor?.NodeRid} "
                + $"current_generation={currentActor?.Generation} "
                + $"direct={routeContext.IsDirectRoute} "
                + $"hop={routeContext.MessageFollowHopCount} "
                + $"target_node_generation={routeContext.TargetNodeGeneration} "
                + $"authority_generation={routeContext.AuthorityOwnerGeneration} "
                + $"owner_lease={routeContext.OwnerLeaseGeneration}");
        }
        if (route == ZLinkActorFrameRoute.MessageFollow)
        {
            ValidateMessageFollowRoute(messageFollowRoute!.Value, routeContext);
            runtime.ActorMessageFollower.Enqueue(
                messageFollowRoute.Value,
                sourceNodeRid,
                sourceSessionRid,
                requestId,
                flags,
                routeContext,
                header,
                body,
                sourceNodeGeneration,
                requestSource,
                directReply,
                applicationMetadata);
        }
        if (route is ZLinkActorFrameRoute.Stale
            or ZLinkActorFrameRoute.MessageFollowExpired)
        {
            runtime.LogActorHandoff(
                route == ZLinkActorFrameRoute.MessageFollowExpired
                    ? "message_follow_expired"
                    : "message_follow_rejected");
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor ref '{frameActor.ActorId}' generation '{frameActor.Generation}' is stale.");
        }

        if (route == ZLinkActorFrameRoute.MessageFollow)
        {
            runtime.LogActorHandoff($"message_follow_relay actor={frameActor.ActorId}");
        }

        return route == ZLinkActorFrameRoute.MessageFollow;
    }

    private static void ValidateMessageFollowRoute(
        ZLinkActorMessageFollowRoute messageFollowRoute,
        ZLinkBackendActorRouteContext route)
    {
        if (!messageFollowRoute.Lease.IsCommitted)
            throw Stale(messageFollowRoute, "the source-to-target route is not committed");
        if (!route.IsDirectRoute)
            return;
        if (route.MessageFollowHopCount >= 8)
            throw Stale(messageFollowRoute, "the Message Follow chain reached the 8-hop limit");
        if (route.TargetNodeGeneration != messageFollowRoute.SourceNodeGeneration
            || route.AuthorityOwnerGeneration
               != messageFollowRoute.SourceAuthorityOwnerGeneration
            || route.OwnerLeaseGeneration
               != messageFollowRoute.SourceOwnerLeaseGeneration)
            throw Stale(messageFollowRoute, "the incoming route fence does not match the committed source");
    }

    private static ZLinkFrameworkException Stale(
        ZLinkActorMessageFollowRoute messageFollowRoute,
        string reason) =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            $"Actor ref '{messageFollowRoute.SourceActor.ActorId}' cannot use Message Follow because {reason}.");

}
