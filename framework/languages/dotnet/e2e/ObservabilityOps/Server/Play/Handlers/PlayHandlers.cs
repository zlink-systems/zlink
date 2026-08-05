using ObservabilityOps.Server.Play.Infrastructure;
using ObservabilityOps.Server.Play.Spots;
using ObservabilityOps.Server.Play.Support;
using ObservabilityOps.Server.Support;
using ObservabilityOps.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace ObservabilityOps.Server.Play.Handlers;

internal sealed class EnsurePlayerHandler(IZLinkActorManager actors)
    : IZLinkSpotRequestHandler<PlayEntrySpot, EnsurePlayerReq, EnsurePlayerRes>
{
    public async ValueTask<EnsurePlayerRes> HandleAsync(PlayEntrySpot spot, EnsurePlayerReq request,
        CancellationToken cancellationToken)
    {
        _ = spot;
        var actor = (await actors.GetOrCreate(request.ActorId, ObservabilityNames.PlayerActorType)
            .Request(request).Async(cancellationToken)) switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Actor creation was rejected.")
        };
        return new EnsurePlayerRes(
            actor.ActorId,
            actor.NodeRid.ToString(),
            actor.ObjectGeneration);
    }
}

internal sealed class PlayBoundedOperationHandler(BoundedOperationGate gate)
    : IZLinkSpotRequestHandler<RoomSpot, PlayBoundedOperationReq, PlayBoundedOperationRes>
{
    public async ValueTask<PlayBoundedOperationRes> HandleAsync(
        RoomSpot spot,
        PlayBoundedOperationReq request,
        CancellationToken cancellationToken)
    {
        await gate.EnterAsync(cancellationToken);
        return new PlayBoundedOperationRes(request.Marker, spot.Context.NodeRid.ToString());
    }
}

internal sealed class JoinRoomHandler
    : IZLinkEntrySpotActorRequestHandler<PlayEntrySpot, PlayerActor, JoinRoomReq, JoinRoomRes>
{
    public ValueTask<JoinRoomRes> HandleAsync(PlayEntrySpot spot, PlayerActor actor,
        IZLinkMessageContext context, JoinRoomReq request, CancellationToken cancellationToken)
    {
        _ = spot;
        _ = context;
        return PlayerRoomJoin.Defer(
            actor, request, spot.Context.NodeRid.ToString(), cancellationToken);
    }
}

internal sealed class MoveRoomHandler
    : IZLinkSpotActorRequestHandler<RoomSpot, PlayerActor, JoinRoomReq, JoinRoomRes>
{
    public ValueTask<JoinRoomRes> HandleAsync(RoomSpot spot, PlayerActor actor,
        IZLinkMessageContext context, JoinRoomReq request, CancellationToken cancellationToken)
    {
        _ = spot;
        _ = context;
        return PlayerRoomJoin.Defer(
            actor, request, spot.Context.NodeRid.ToString(), cancellationToken);
    }
}

internal static class PlayerRoomJoin
{
    internal static ValueTask<JoinRoomRes> Defer(
        PlayerActor actor,
        JoinRoomReq request,
        string currentNodeRid,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        actor.TrackRoomJoin(request.RoomRid);
        actor.Context.JoinSpot(request.RoomRid, request).Defer();
        return ValueTask.FromResult(new JoinRoomRes(
            actor.ActorId,
            request.RoomRid,
            currentNodeRid));
    }
}

internal sealed class ReturnToLobbyHandler
    : IZLinkSpotActorRequestHandler<RoomSpot, PlayerActor, ReturnToLobbyReq, ReturnToLobbyRes>
{
    public ValueTask<ReturnToLobbyRes> HandleAsync(RoomSpot spot, PlayerActor actor,
        IZLinkMessageContext context, ReturnToLobbyReq request, CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        actor.TrackEntrySpotJoin(request.Marker);
        actor.Context.JoinEntrySpot(request).Defer();
        return ValueTask.FromResult(new ReturnToLobbyRes(
            actor.ActorId,
            spot.Context.NodeRid.ToString(),
            request.Marker));
    }
}

internal sealed class GameActionHandler(EvidenceStore evidence)
    : IZLinkSpotActorRequestHandler<RoomSpot, PlayerActor, GameActionReq, GameActionRes>
{
    public async ValueTask<GameActionRes> HandleAsync(RoomSpot spot, PlayerActor actor,
        IZLinkMessageContext context, GameActionReq request, CancellationToken cancellationToken)
    {
        _ = context;
        if (request.WorkMilliseconds > 0)
            await Task.Delay(TimeSpan.FromMilliseconds(request.WorkMilliseconds), cancellationToken);
        evidence.Add($"game-action|actor={actor.ActorId}|room={spot.Context.SpotId}|marker={request.Marker}");
        return new GameActionRes(actor.ActorId, spot.Context.SpotId.ToString(),
            spot.Context.NodeRid.ToString(), request.Marker);
    }
}
