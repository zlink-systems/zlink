using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Handlers;

[ZLinkSpotTimerHandler("bingo-draw", 200)]
internal sealed class BingoRoomDrawTimerHandler : IZLinkSpotTimerHandler<BingoRoom>
{
    public async ValueTask HandleAsync(
        BingoRoom spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        _ = tick;
        if (!spot.IsReadyToDraw) return;

        var change = spot.DrawNextNumber();
        await spot.PublishAsync(change, cancellationToken);
        if (change.ShouldStopDrawTimer)
        {
            await spot.LeaveFinishedActorsAsync(cancellationToken);
            // This is the final Framework operation in the completed round turn.
            spot.DeferRelocationAtRoundBoundary();
        }
    }
}
