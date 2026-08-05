using Bingo.Shared.Contracts;

namespace Bingo.Server.Matchmaking.Application;

internal interface IBingoMatchReservationStore
{
    ValueTask<ReserveBingoRoomRes> ReserveAsync(
        ReserveBingoRoomReq request,
        CancellationToken cancellationToken);
}
