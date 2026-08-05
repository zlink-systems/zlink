using TicTacToe.Server.Play.Infrastructure.ZLink.Actors;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Spots.TicTacToeGameSpot.Handlers;

[ZLinkSpotActorRequestHandler(nameof(LeaveGameMsg))]
internal sealed class PlayActorLeaveGameHandler(ILogger<PlayActorLeaveGameHandler> logger)
    : IZLinkSpotActorRequestHandler<
        TicTacToeGame,
        PlayActor,
        LeaveGameMsg,
        LeaveGameRes>
{
    public async ValueTask<LeaveGameRes> HandleAsync(
        TicTacToeGame spot,
        PlayActor actor,
        IZLinkMessageContext context,
        LeaveGameMsg message,
        CancellationToken cancellationToken)
    {
        var roomId = actor.RequireJoinedRoom();
        if (!string.Equals(roomId, message.RoomId, StringComparison.Ordinal))
            throw new InvalidOperationException($"Actor is joined to '{roomId}', not '{message.RoomId}'.");

        logger.LogInformation(
            "actor: LeaveGameMsg received. actor={ActorId}, roomId={RoomId}",
            actor.ActorId,
            message.RoomId);

        await spot.LeaveGameAsync(actor, message.RoomId, cancellationToken);

        logger.LogInformation(
            "actor: LeaveGameMsg completed. actor={ActorId}, roomId={RoomId}",
            actor.ActorId,
            message.RoomId);
        return new LeaveGameRes(true);
    }
}
