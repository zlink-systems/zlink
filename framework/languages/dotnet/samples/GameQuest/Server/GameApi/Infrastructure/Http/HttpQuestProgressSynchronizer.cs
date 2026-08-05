using GameQuest.GameApi.Application;
using GameQuest.Server.Configuration;
using GameQuest.Shared;
using Zlink.Framework.Contracts.Spots;

namespace GameQuest.GameApi.Infrastructure.ZLink;

internal sealed class ZLinkQuestProgressSynchronizer(
    IZLinkSpotClient spots) : IQuestProgressSynchronizer
{
    public async ValueTask<SyncQuestProgressRes> SyncAsync(
        string playerId,
        CancellationToken cancellationToken)
    {
        return await spots.RequestToSpot(playerId,
                new SyncQuestProgressReq(playerId))
            .InstanceSpot(SampleNames.PlayerQuestSpotType)
            .InMesh(SampleNames.MeshName)
            .Async<SyncQuestProgressRes>(cancellationToken);
    }
}
