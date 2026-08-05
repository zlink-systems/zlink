using GameQuest.Shared;

namespace GameQuest.GameApi.Application;

internal sealed class JoinQuestSessionUseCase(IQuestSessionStore sessions)
{
    public async ValueTask<JoinSessionRes> ExecuteAsync(
        string playerId,
        CancellationToken cancellationToken)
    {
        var projection = await sessions.ReadProjectionAsync(playerId, cancellationToken);
        return new JoinSessionRes(playerId, projection);
    }
}

internal interface IQuestProgressSynchronizer
{
    ValueTask<SyncQuestProgressRes> SyncAsync(
        string playerId,
        CancellationToken cancellationToken);
}

internal interface IQuestSessionStore
{
    ValueTask<QuestProgress[]> ReadProjectionAsync(
        string playerId,
        CancellationToken cancellationToken);
}
