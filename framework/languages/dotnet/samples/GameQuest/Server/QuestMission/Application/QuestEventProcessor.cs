using GameQuest.QuestMission.Domain;

namespace GameQuest.QuestMission.Application;

internal sealed record QuestProcessorIdentity(string MissionName);

internal sealed record GameplaySnapshot(string PlayerId, int KillCount, long Version);

internal sealed class QuestEventProcessor(
    IQuestStore store,
    IGameApiSnapshotClient snapshots,
    IQuestProgressNotifier notifications,
    QuestProcessorIdentity identity,
    ILogger<QuestEventProcessor> logger)
{
    public async ValueTask ProcessAsync(
        GameplayFact gameplayFact,
        CancellationToken cancellationToken)
    {
        var definition = gameplayFact.EventType == "SnapshotKillCount"
            ? QuestCatalog.All.First(candidate => candidate.QuestId == "first-hunt")
            : QuestCatalog.Match(gameplayFact);
        if (definition is null) return;

        var stream = await store.ReadQuestStreamAsync(
            gameplayFact.PlayerId,
            definition.QuestId,
            cancellationToken);
        var aggregate = QuestProgressAggregate.Rehydrate(definition, stream);
        var decision = aggregate.Decide(gameplayFact);
        if (decision is null) return;
        if (!await store.AppendAndProjectAsync(
                decision.State,
                decision.Events,
                cancellationToken)) return;

        var projection = await store.ReadProjectionAsync(gameplayFact.PlayerId, cancellationToken);
        var completedQuestId = decision.Events.OfType<QuestCompleted>().Any()
            ? definition.QuestId
            : null;
        var notified = await NotifyBoundGameApiAsync(
            gameplayFact.PlayerId,
            projection,
            completedQuestId,
            cancellationToken);

        logger.LogInformation(
            "gamequest mission processed mission={Mission} player={PlayerId} quest={QuestId} source={SourceEventId} events={EventCount} notified={Notified}",
            identity.MissionName,
            gameplayFact.PlayerId,
            definition.QuestId,
            gameplayFact.EventId,
            decision.Events.Count,
            notified);
    }

    public async ValueTask<QuestProgressState[]> SyncAsync(
        string playerId,
        CancellationToken cancellationToken)
    {
        var snapshot = await snapshots.ReadSnapshotAsync(playerId, cancellationToken);
        if (snapshot.KillCount > 0)
            await ProcessAsync(
                new GameplayFact(
                    $"{playerId}-snapshot-{snapshot.Version}",
                    playerId,
                    "SnapshotKillCount",
                    "kills",
                    snapshot.KillCount,
                    "api-a",
                    DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()),
                cancellationToken);

        return await store.ReadProjectionAsync(playerId, cancellationToken);
    }

    private async ValueTask<bool> NotifyBoundGameApiAsync(
        string playerId,
        IReadOnlyList<QuestProgressState> projection,
        string? completedQuestId,
        CancellationToken cancellationToken)
    {
        var result = await notifications.NotifyAsync(
            playerId,
            projection,
            completedQuestId,
            cancellationToken);
        if (result.FailureStatus is not null)
            logger.LogWarning(
                "gamequest mission projection kept while stream notify failed. player={PlayerId} status={StatusCode}",
                playerId,
                result.FailureStatus);
        else if (result.UnavailableError is not null)
            logger.LogWarning(
                result.UnavailableError,
                "gamequest mission projection kept while stream notify endpoint was unavailable. player={PlayerId}",
                playerId);

        return result.Delivered;
    }
}

internal interface IQuestStore
{
    ValueTask<QuestProgressState[]> ReadProjectionAsync(
        string playerId,
        CancellationToken cancellationToken);

    ValueTask<QuestDomainEvent[]> ReadQuestStreamAsync(
        string playerId,
        string questId,
        CancellationToken cancellationToken);

    ValueTask<bool> AppendAndProjectAsync(
        QuestProgressState projection,
        IReadOnlyList<QuestDomainEvent> events,
        CancellationToken cancellationToken);
}

internal interface IGameApiSnapshotClient
{
    ValueTask<GameplaySnapshot> ReadSnapshotAsync(
        string playerId,
        CancellationToken cancellationToken);
}

internal interface IQuestProgressNotifier
{
    ValueTask<QuestProgressNotifyResult> NotifyAsync(
        string playerId,
        IReadOnlyList<QuestProgressState> projection,
        string? completedQuestId,
        CancellationToken cancellationToken);
}

internal sealed record QuestProgressNotifyResult(
    bool Delivered,
    int? FailureStatus,
    Exception? UnavailableError);
