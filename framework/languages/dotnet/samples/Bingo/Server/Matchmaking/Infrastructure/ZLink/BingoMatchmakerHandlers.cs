using Bingo.Server.Matchmaking.Application;
using Bingo.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace Bingo.Server.Matchmaking.Infrastructure.ZLink;

internal sealed class ReserveBingoRoomHandler(IBingoMatchReservationStore reservations)
    : IZLinkSpotRequestHandler<BingoMatchmaker, ReserveBingoRoomReq, ReserveBingoRoomRes>
{
    public async ValueTask<ReserveBingoRoomRes> HandleAsync(
        BingoMatchmaker spot,
        ReserveBingoRoomReq request,
        CancellationToken cancellationToken)
    {
        spot.RecordActivity();
        return await reservations.ReserveAsync(request, cancellationToken);
    }
}

internal sealed class BingoMatchmakerIdleTimer
    : IZLinkSpotTimerHandler<BingoMatchmaker>
{
    public async ValueTask HandleAsync(
        BingoMatchmaker spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        _ = tick;
        if (DateTimeOffset.UtcNow - spot.LastActivity >= TimeSpan.FromSeconds(30))
            await spot.Context.CloseAsync(cancellationToken);
    }
}
