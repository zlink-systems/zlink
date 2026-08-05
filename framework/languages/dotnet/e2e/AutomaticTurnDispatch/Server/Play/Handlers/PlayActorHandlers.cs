using Systems.Zlink;
using AutomaticTurnDispatch.Server.Play.Spots;
using AutomaticTurnDispatch.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace AutomaticTurnDispatch.Server.Play.Handlers;

[ZLinkSpotActorRequestHandler("ActorAwaitReq")]
internal sealed class EntryActorAwaitHandler(
    EvidenceStore evidence,
    IZLinkRouteClient routeClient)
    : IZLinkEntrySpotActorRequestHandler<AwaitEntrySpot, AwaitActor, ActorAwaitReq, ActorAwaitRes>
{
    public async ValueTask<ActorAwaitRes> HandleAsync(
        AwaitEntrySpot entrySpot,
        AwaitActor actor,
        IZLinkMessageContext context,
        ActorAwaitReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        var mailboxId = $"actor:{actor.ActorId}";
        evidence.Add(
            $"actor-await-started|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        var call = routeClient.RequestToChannel(
                AutomaticTurnDispatchNames.DelayChannel,
                new DelayReq(request.RequestId, request.DelayMs, $"actor-{actor.ActorId}"))
            .Timeout(TimeSpan.FromSeconds(5));
        evidence.Add(
            $"actor-await-{(request.Terminator == "yield" ? "released" : "held")}|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        await TurnTerminator.Complete<DelayRes>(call, request.Terminator, cancellationToken);
        evidence.Add(
            $"actor-await-resumed|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        evidence.Add(
            $"actor-await-completed|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        return ActorReplies.Reply("probe-B", request.RequestId, actor, entrySpot, "actor-await-completed");
    }
}

[ZLinkSpotActorRequestHandler("ActorFastReq")]
internal sealed class EntryActorFastHandler(EvidenceStore evidence)
    : IZLinkEntrySpotActorRequestHandler<AwaitEntrySpot, AwaitActor, ActorFastReq, ActorAwaitRes>
{
    public ValueTask<ActorAwaitRes> HandleAsync(
        AwaitEntrySpot entrySpot,
        AwaitActor actor,
        IZLinkMessageContext context,
        ActorFastReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        var mailboxId = $"actor:{actor.ActorId}";
        evidence.Add(
            $"actor-fast-started|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}"
            + $"|marker={request.Marker}|handler=actor");
        evidence.Add(
            $"actor-fast-completed|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}"
            + $"|marker={request.Marker}|handler=actor");
        return ValueTask.FromResult(ActorReplies.Reply("probe-B", request.RequestId, actor, entrySpot, request.Marker));
    }
}

[ZLinkSpotActorRequestHandler("ActorAwaitReq")]
internal sealed class SpotActorAwaitHandler(
    EvidenceStore evidence,
    IZLinkRouteClient routeClient)
    : IZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor, ActorAwaitReq, ActorAwaitRes>
{
    public async ValueTask<ActorAwaitRes> HandleAsync(
        AwaitProbeSpot spot,
        AwaitActor actor,
        IZLinkMessageContext context,
        ActorAwaitReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        var mailboxId = $"actor:{actor.ActorId}";
        evidence.Add(
            $"actor-await-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        var call = routeClient.RequestToChannel(
                AutomaticTurnDispatchNames.DelayChannel,
                new DelayReq(request.RequestId, request.DelayMs, $"actor-{actor.ActorId}"))
            .Timeout(TimeSpan.FromSeconds(5));
        evidence.Add(
            $"actor-await-{(request.Terminator == "yield" ? "released" : "held")}|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        await TurnTerminator.Complete<DelayRes>(call, request.Terminator, cancellationToken);
        evidence.Add(
            $"actor-await-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        evidence.Add(
            $"actor-await-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        return ActorReplies.Reply("probe-B", request.RequestId, actor, spot, "actor-await-completed");
    }
}

[ZLinkSpotActorRequestHandler("ActorFastReq")]
internal sealed class SpotActorFastHandler(EvidenceStore evidence)
    : IZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor, ActorFastReq, ActorAwaitRes>
{
    public ValueTask<ActorAwaitRes> HandleAsync(
        AwaitProbeSpot spot,
        AwaitActor actor,
        IZLinkMessageContext context,
        ActorFastReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        var mailboxId = $"actor:{actor.ActorId}";
        evidence.Add(
            $"actor-fast-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}"
            + $"|marker={request.Marker}|handler=actor");
        evidence.Add(
            $"actor-fast-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}"
            + $"|marker={request.Marker}|handler=actor");
        return ValueTask.FromResult(ActorReplies.Reply("probe-B", request.RequestId, actor, spot, request.Marker));
    }
}

[ZLinkSpotActorRequestHandler("ActorPushAwaitReq")]
internal sealed class SpotActorPushAwaitHandler(
    EvidenceStore evidence,
    IZLinkRouteClient routeClient)
    : IZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor, ActorPushAwaitReq, ActorAwaitRes>
{
    public async ValueTask<ActorAwaitRes> HandleAsync(
        AwaitProbeSpot spot,
        AwaitActor actor,
        IZLinkMessageContext context,
        ActorPushAwaitReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        var mailboxId = $"actor:{actor.ActorId}";
        evidence.Add(
            $"actor-push-await-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        var call = routeClient.RequestToChannel(
                AutomaticTurnDispatchNames.DelayChannel,
                new DelayReq(request.RequestId, request.DelayMs, $"actor-push-{actor.ActorId}"))
            .Timeout(TimeSpan.FromSeconds(5));
        evidence.Add(
            $"actor-push-await-released|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        await call.Async<DelayRes>(cancellationToken);
        evidence.Add(
            $"actor-push-await-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        await actor.Context.BoundSession.Send(
                new ActorPushNotify(
                    actor.ActorId,
                    request.RequestId,
                    request.Value,
                    spot.Context.NodeRid.ToString())).Async(cancellationToken);
        evidence.Add(
            $"actor-push-await-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        return ActorReplies.Reply("probe-D4", request.RequestId, actor, spot, "actor-push-await-completed");
    }
}

[ZLinkSpotActorRequestHandler("ActorJoinAwaitReq")]
internal sealed class EntryActorJoinAwaitHandler(EvidenceStore evidence)
    : IZLinkEntrySpotActorRequestHandler<AwaitEntrySpot, AwaitActor, ActorJoinAwaitReq, ActorAwaitRes>
{
    public ValueTask<ActorAwaitRes> HandleAsync(
        AwaitEntrySpot entrySpot,
        AwaitActor actor,
        IZLinkMessageContext context,
        ActorJoinAwaitReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        var mailboxId = $"actor:{actor.ActorId}";
        evidence.Add(
            $"actor-join-await-started|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|target={request.TargetSpotRid}");
        actor.TrackJoin(request.RequestId, "actor-join-await");
        var call = actor.Context.JoinSpot(
            request.TargetSpotRid,
            ZLinkMessage.From(new JoinDelayReq(
                request.RequestId,
                350,
                "actor-join-await")));
        call.Defer();
        evidence.Add(
            $"actor-join-await-released|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|target={request.TargetSpotRid}");
        return ValueTask.FromResult(
            ActorReplies.Reply("probe-B3", request.RequestId, actor, entrySpot, "actor-join-await-released"));
    }
}

[ZLinkSpotActorRequestHandler("ActorJoinAwaitReq")]
internal sealed class SpotActorJoinAwaitHandler(EvidenceStore evidence)
    : IZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor, ActorJoinAwaitReq, ActorAwaitRes>
{
    public ValueTask<ActorAwaitRes> HandleAsync(
        AwaitProbeSpot spot,
        AwaitActor actor,
        IZLinkMessageContext context,
        ActorJoinAwaitReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        evidence.Add(
            $"actor-join-started|rid={evidence.Rid}|spot={spot.Context.SpotId}|actor={actor.ActorId}"
            + $"|request={request.RequestId}|target={request.TargetSpotRid}");
        actor.TrackJoin(request.RequestId, "actor-join");
        actor.Context.JoinSpot(
                request.TargetSpotRid,
                ZLinkMessage.From(new JoinDelayReq(
                    request.RequestId,
                    25,
                    "actor-join")))
            .Defer();
        evidence.Add(
            $"actor-join-deferred|rid={evidence.Rid}|spot={spot.Context.SpotId}|actor={actor.ActorId}"
            + $"|request={request.RequestId}|target={request.TargetSpotRid}");
        return ValueTask.FromResult(
            ActorReplies.Reply("TD-E2", request.RequestId, actor, spot, "actor-join-deferred"));
    }
}

[ZLinkSpotActorRequestHandler("ActorPushAwaitReq")]
internal sealed class EntryActorPushAwaitHandler(
    EvidenceStore evidence,
    IZLinkRouteClient routeClient)
    : IZLinkEntrySpotActorRequestHandler<AwaitEntrySpot, AwaitActor, ActorPushAwaitReq, ActorAwaitRes>
{
    public async ValueTask<ActorAwaitRes> HandleAsync(
        AwaitEntrySpot entrySpot,
        AwaitActor actor,
        IZLinkMessageContext context,
        ActorPushAwaitReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        var mailboxId = $"actor:{actor.ActorId}";
        evidence.Add(
            $"actor-push-await-started|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        var call = routeClient.RequestToChannel(
                AutomaticTurnDispatchNames.DelayChannel,
                new DelayReq(request.RequestId, request.DelayMs, $"actor-push-{actor.ActorId}"))
            .Timeout(TimeSpan.FromSeconds(5));
        evidence.Add(
            $"actor-push-await-released|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        await call.Async<DelayRes>(cancellationToken);
        evidence.Add(
            $"actor-push-await-resumed|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        await actor.Context.BoundSession.Send(
                new ActorPushNotify(
                    actor.ActorId,
                    request.RequestId,
                    request.Value,
                    entrySpot.Context.NodeRid.ToString())).Async(cancellationToken);
        evidence.Add(
            $"actor-push-await-completed|rid={evidence.Rid}|spot={entrySpot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}|handler=actor");
        return ActorReplies.Reply("probe-D4", request.RequestId, actor, entrySpot, "actor-push-await-completed");
    }
}
