using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Handlers;

internal sealed class BingoRoomCloseTimerHandler(ILogger<BingoRoomCloseTimerHandler> logger)
    : IZLinkSpotTimerHandler<BingoRoom>
{
    public ValueTask HandleAsync(
        BingoRoom spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken
    )
    {
        _ = tick;
        if (!spot.TryRequestClose())
            return ValueTask.CompletedTask;
        logger.LogInformation("bingo-lifecycle room-close spot={SpotId}", spot.Context.SpotId);
        _ = spot.Context.CloseAsync(cancellationToken);
        return ValueTask.CompletedTask;
    }
}
