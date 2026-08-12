using TicTacToe.Server.Play.Infrastructure.ZLink.Actors;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Spots.TicTacToeGameSpot.Handlers;

internal sealed class PlayActorGetCurrentGameStateHandler(
    ILogger<PlayActorGetCurrentGameStateHandler> logger)
    : IZLinkSpotActorSendHandler<
        TicTacToeGame,
        PlayActor,
        JoinGameMsg>
{
    public async ValueTask HandleAsync(
        TicTacToeGame spot,
        PlayActor actor,
        IZLinkMessageContext context,
        JoinGameMsg message,
        CancellationToken cancellationToken)
    {
        var state = spot.GetCurrentState(actor, message.RoomId);
        await actor.Context.BoundSession.Send(new JoinGameNotify(state))
            .Async(cancellationToken);

        logger.LogInformation(
            "game spot: current state returned to reconnected actor. actor={ActorId}, roomId={RoomId}",
            actor.ActorId,
            state.RoomId);
    }
}
