using Bingo.Server.Play.Infrastructure.ZLink.Actors;
using Bingo.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Handlers;

internal sealed class StopObservingBingoEventsHandler
    : IZLinkSpotActorRequestHandler<BingoRoom, PlayerActor, StopObservingBingoEventsReq, StopObservingBingoEventsRes>
{
    public async ValueTask<StopObservingBingoEventsRes> HandleAsync(
        BingoRoom spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        StopObservingBingoEventsReq message,
        CancellationToken cancellationToken)
    {
        var stopped = await spot.StopObservingAsync(actor, message.RoomId, cancellationToken);
        return new StopObservingBingoEventsRes
        {
            Stopped = stopped
        };
    }
}
