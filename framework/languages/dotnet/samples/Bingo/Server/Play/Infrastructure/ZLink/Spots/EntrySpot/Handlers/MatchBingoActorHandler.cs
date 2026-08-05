using Bingo.Server.Configuration;
using Bingo.Server.Play.Infrastructure.ZLink.Actors;
using Bingo.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.EntrySpot.Handlers;

internal sealed class MatchBingoActorHandler(
    ILogger<MatchBingoActorHandler> logger)
    : IZLinkEntrySpotActorSendHandler<BingoEntrySpot, PlayerActor, MatchBingoReq>
{
    public async ValueTask HandleAsync(
        BingoEntrySpot entrySpot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MatchBingoReq message,
        CancellationToken cancellationToken)
    {
        logger.LogInformation("match: actor request. actor={ActorId}, mode={Mode}", actor.ActorId, message.Mode);
        var apiRequest = new MatchBingoApiReq
        {
            ActorId = actor.ActorId,
            DisplayName = actor.DisplayName,
            Mode = message.Mode
        };
        var matched = await entrySpot.Context.Outbound
            .RequestToChannel(SampleNames.ApiChannel, apiRequest)
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<MatchBingoApiRes>(cancellationToken);
        logger.LogInformation("match: room allocated. actor={ActorId}, room={RoomId}", actor.ActorId, matched.RoomId);

        actor.TrackDeferredJoin(matched.RoomId, observeOnly: false);
        actor.Context.JoinSpot(
                matched.RoomId,
                new BingoRoomJoinReq
                {
                    RoomId = matched.RoomId,
                    ActorId = actor.ActorId,
                    DisplayName = actor.DisplayName,
                    ObserveOnly = false
                })
            .Defer();
        logger.LogInformation("match: actor join scheduled. actor={ActorId}, room={RoomId}", actor.ActorId,
            matched.RoomId);
    }
}
