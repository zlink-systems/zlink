using AutomaticTurnDispatch.Server.Play.Spots;
using AutomaticTurnDispatch.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace AutomaticTurnDispatch.Server.Play.Handlers;

[ZLinkSpotActorRequestHandler("PerActorAwaitReq")]
internal sealed class PerActorAwaitHandler(
    EvidenceStore evidence,
    IZLinkRouteClient routeClient)
    : IZLinkSpotActorRequestHandler<PerActorAwaitSpot, AwaitActor, PerActorAwaitReq, ActorAwaitRes>
{
    public async ValueTask<ActorAwaitRes> HandleAsync(
        PerActorAwaitSpot spot,
        AwaitActor actor,
        IZLinkMessageContext context,
        PerActorAwaitReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        var mailboxId = $"actor:{actor.ActorId}";
        evidence.Add(
            $"per-actor-await-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}");
        var call = routeClient.RequestToChannel(
                AutomaticTurnDispatchNames.DelayChannel,
                new DelayReq(request.RequestId, request.DelayMs, $"per-actor-{actor.ActorId}"))
            .Timeout(TimeSpan.FromSeconds(5));
        evidence.Add(
            $"per-actor-await-{(request.Terminator == "yield" ? "released" : "held")}|rid={evidence.Rid}"
            + $"|spot={spot.Context.SpotId}|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}");
        await TurnTerminator.Complete<DelayRes>(call, request.Terminator, cancellationToken);
        evidence.Add(
            $"per-actor-await-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}");
        evidence.Add(
            $"per-actor-await-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|mailbox={mailboxId}|request={request.RequestId}");
        return ActorReplies.Reply("TD-D4", request.RequestId, actor, spot, "per-actor-await-completed");
    }
}

[ZLinkSpotActorRequestHandler("PerActorFastReq")]
internal sealed class PerActorFastHandler(EvidenceStore evidence)
    : IZLinkSpotActorRequestHandler<PerActorAwaitSpot, AwaitActor, PerActorFastReq, ActorAwaitRes>
{
    public ValueTask<ActorAwaitRes> HandleAsync(
        PerActorAwaitSpot spot,
        AwaitActor actor,
        IZLinkMessageContext context,
        PerActorFastReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"per-actor-fast-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|request={request.RequestId}|marker={request.Marker}");
        evidence.Add(
            $"per-actor-fast-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|actor={actor.ActorId}|request={request.RequestId}|marker={request.Marker}");
        return ValueTask.FromResult(
            ActorReplies.Reply("TD-D4", request.RequestId, actor, spot, request.Marker));
    }
}
