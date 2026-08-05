using TicTacToe.Server.Play.Infrastructure.ZLink.Actors;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Spots.EntrySpot.Handlers;

[ZLinkSpotActorSendHandler(nameof(JoinGameReq))]
// --8<-- [start:doc-join-defer]
internal sealed class PlayActorJoinGameHandler(ILogger<PlayActorJoinGameHandler> logger)
    : IZLinkEntrySpotActorSendHandler<PlayEntrySpot, PlayActor, JoinGameReq>
{
    public ValueTask HandleAsync(
        PlayEntrySpot entrySpot,
        PlayActor actor,
        IZLinkMessageContext context,
        JoinGameReq message,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "actor: JoinGameReq received. actor={ActorId}, roomId={RoomId}",
            actor.ActorId,
            message.RoomId);

        actor.TrackDeferredJoin(message.RoomId);
        actor.Context.JoinSpot(
                message.RoomId,
                new TicTacToeGameJoinReq(message.RoomId, actor.RequirePlayer()))
            .Defer();
        logger.LogInformation(
            "actor: room join scheduled. actor={ActorId}, roomId={RoomId}",
            actor.ActorId,
            message.RoomId);
        return ValueTask.CompletedTask;
    }
}
// --8<-- [end:doc-join-defer]
