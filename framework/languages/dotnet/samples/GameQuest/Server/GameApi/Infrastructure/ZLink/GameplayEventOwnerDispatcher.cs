using GameQuest.GameApi.Application;
using GameQuest.GameApi.Domain;
using GameQuest.Server.Configuration;
using GameQuest.Shared;
using System.Text.Json;
using Zlink.Framework.Contracts.Spots;

namespace GameQuest.GameApi.Infrastructure.ZLink;

internal sealed class GameplayEventOwnerDispatcher(
    IZLinkSpotClient spots) : IGameplayEventOwnerDispatcher
{
    public async ValueTask<string> DispatchAsync(
        GameplayEvent gameplayEvent,
        CancellationToken cancellationToken)
    {
        await spots.SendToSpot(gameplayEvent.PlayerId,
                new GameplayMsg(
                    gameplayEvent.EventId,
                    gameplayEvent.PlayerId,
                    gameplayEvent.EventType,
                    JsonSerializer.SerializeToElement(new GameplayPayload(
                        gameplayEvent.Value,
                        gameplayEvent.Count,
                        gameplayEvent.SourceApi)),
                    gameplayEvent.CreatedAtUnixMs))
            // A missing player owner is activated by this same gameplay
            // message; the caller never resolves or chooses a physical node.
            .InstanceSpot(SampleNames.PlayerQuestSpotType)
            .InMesh(SampleNames.MeshName)
            .Async(cancellationToken);
        return gameplayEvent.PlayerId;
    }

    private sealed record GameplayPayload(string Value, int Count, string SourceApi);
}
