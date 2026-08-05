using GameQuest.QuestMission.Application;
using GameQuest.QuestMission.Domain;
using GameQuest.QuestMission.Infrastructure.Store;
using GameQuest.Server.Configuration;
using GameQuest.Shared;
using System.Net.Http.Json;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Errors;

namespace GameQuest.QuestMission.Infrastructure.ZLink;

internal sealed class HttpGameApiSnapshotClient(GameQuestTopology topology) : IGameApiSnapshotClient
{
    private static readonly HttpClient Client = new();

    public async ValueTask<GameplaySnapshot> ReadSnapshotAsync(
        string playerId,
        CancellationToken cancellationToken)
    {
        var response = await Client.GetFromJsonAsync<GameplaySnapshotResponse>(
                           $"{topology.GameApiAHttpBaseUrl}/self-check/gameplay/snapshot/{Uri.EscapeDataString(playerId)}",
                           cancellationToken)
                       ?? throw new InvalidOperationException("Game API returned an empty gameplay snapshot.");
        return new GameplaySnapshot(
            response.PlayerId,
            response.KillCount,
            response.SnapshotVersion);
    }

    private sealed record GameplaySnapshotResponse(string PlayerId, int KillCount, long SnapshotVersion);
}

internal sealed class ZLinkQuestProgressNotifier(IZLinkActorClient actors) : IQuestProgressNotifier
{
    public async ValueTask<QuestProgressNotifyResult> NotifyAsync(
        string playerId,
        IReadOnlyList<QuestProgressState> projection,
        string? completedQuestId,
        CancellationToken cancellationToken)
    {
        try
        {
            var contracts = projection.Select(QuestContractMapper.ToContract).ToArray();
            foreach (var progress in contracts)
                await actors.SendToActor(playerId, new QuestProgressNotify(playerId, progress))
                    .Async(cancellationToken);

            if (!string.IsNullOrWhiteSpace(completedQuestId))
            {
                var completed = contracts.First(progress => progress.QuestId == completedQuestId);
                await actors.SendToActor(playerId, new QuestCompletedNotify(playerId, completed, true))
                    .Async(cancellationToken);
            }

            return new QuestProgressNotifyResult(true, null, null);
        }
        catch (ZLinkFrameworkException error)
        {
            return new QuestProgressNotifyResult(false, null, error);
        }
    }
}
