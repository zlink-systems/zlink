using Bingo.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Handlers;

internal sealed class GetBingoObservationHandler
    : IZLinkSpotRequestHandler<BingoRoom, GetBingoObservationReq, GetBingoObservationRes>
{
    public ValueTask<GetBingoObservationRes> HandleAsync(
        BingoRoom spot,
        GetBingoObservationReq request,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(spot.Observation());
}
