using Bingo.Server.Configuration;
using Bingo.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace Bingo.Server.Api.Handlers;

[ZLinkHandlerGroup("api")]
internal sealed class MatchBingoHandler(
    IZLinkSpotClient spotClient,
    IZLinkSpotManager spots,
    ILogger<MatchBingoHandler> logger)
    : IZLinkRequestHandler<MatchBingoApiReq, MatchBingoApiRes>
{
    public async ValueTask<MatchBingoApiRes> HandleAsync(
        MatchBingoApiReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        logger.LogInformation("api match: request. actor={ActorId}, mode={Mode}",
            request.ActorId, request.Mode);
        const string levelBucket = "1-10";
        var allocated = await spotClient
            .RequestToSpot(
                $"match:{levelBucket}",
                new ReserveBingoRoomReq
                {
                    Mode = request.Mode,
                    ActorId = request.ActorId,
                    LevelBucket = levelBucket
                })
            .InstanceSpot(SampleNames.MatchmakerSpotType)
            .InMesh(SampleNames.MatchmakingMeshName)
            .Async<ReserveBingoRoomRes>(cancellationToken);
        logger.LogInformation("api match: allocated. actor={ActorId}, room={RoomId}",
            request.ActorId, allocated.RoomId);
        var created = await spots
            .GetOrCreate(allocated.RoomId, SampleNames.RoomSpotType)
            .InMesh(SampleNames.PlayMeshName)
            .Request(allocated.Settings)
            .Async(cancellationToken);
        logger.LogInformation(
            "api match: room Spot ready. room={RoomId}, state={State}",
            created.Spot.SpotId,
            created.State);

        return new MatchBingoApiRes
        {
            RoomId = allocated.RoomId
        };
    }
}
