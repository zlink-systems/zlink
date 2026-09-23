using TicTacToe.Server.Play.Infrastructure.ZLink.Actors;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Spots.TicTacToeGameSpot.Handlers;

// --8<-- [start:doc-actor-packet-handler]
internal sealed class PlayActorPlaceMarkHandler
    : IZLinkSpotActorRequestHandler<TicTacToeGame, PlayActor, PlaceMarkReq, PlaceMarkRes>
{
    public async ValueTask<PlaceMarkRes> HandleAsync(
        TicTacToeGame spot,
        PlayActor actor,
        IZLinkMessageContext context,
        PlaceMarkReq message,
        CancellationToken cancellationToken
    )
    {
        actor.RequireJoinedRoom();
        return await spot.PlaceMarkAsync(actor, message.Cell, cancellationToken);
    }
}
// --8<-- [end:doc-actor-packet-handler]
