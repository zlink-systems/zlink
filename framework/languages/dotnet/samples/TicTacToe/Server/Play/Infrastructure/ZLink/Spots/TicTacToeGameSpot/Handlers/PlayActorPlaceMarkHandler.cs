using TicTacToe.Server.Play.Infrastructure.ZLink.Actors;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Spots.TicTacToeGameSpot.Handlers;

[ZLinkSpotActorRequestHandler(nameof(PlaceMarkReq))]
// --8<-- [start:doc-actor-packet-handler]
internal sealed class PlayActorPlaceMarkHandler(ILogger<PlayActorPlaceMarkHandler> logger)
    : IZLinkSpotActorRequestHandler<TicTacToeGame, PlayActor, PlaceMarkReq, PlaceMarkRes>
{
    public async ValueTask<PlaceMarkRes> HandleAsync(
        TicTacToeGame spot,
        PlayActor actor,
        IZLinkMessageContext context,
        PlaceMarkReq message,
        CancellationToken cancellationToken)
    {
        var roomId = actor.RequireJoinedRoom();
        logger.LogInformation(
            "actor: PlaceMarkReq received. actor={ActorId}, roomId={RoomId}, cell={Cell}",
            actor.ActorId,
            roomId,
            message.Cell);

        var reply = await spot.PlaceMarkAsync(actor, message.Cell, cancellationToken);

        logger.LogInformation(
            "actor -> client: PlaceMarkRes returned. actor={ActorId}, roomId={RoomId}, board={Board}, status={Status}",
            actor.ActorId,
            reply.State.RoomId,
            reply.State.Board,
            reply.State.Status);
        return reply;
    }
}
// --8<-- [end:doc-actor-packet-handler]
