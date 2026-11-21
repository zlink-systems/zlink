using Bingo.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Handlers;

internal sealed class GetBingoObservationHandler
    : IZLinkSpotRequestHandler<BingoRoom, GetBingoObservationReq, GetBingoObservationRes>
{
    public ValueTask<GetBingoObservationRes> HandleAsync(
        BingoRoom spot,
        IZLinkMessageContext context,
        GetBingoObservationReq request,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(spot.Observation());
}
