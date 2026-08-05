using System.Text.Json;
using GameQuest.GameApi.Application;
using GameQuest.GameApi.Domain;
using GameQuest.Server.Configuration;
using GameQuest.Shared;

namespace GameQuest.GameApi.Infrastructure.Store;

internal sealed class GameQuestStore : IGameplayEventStore, IQuestSessionStore, IAsyncDisposable
{
    private readonly string _keyPrefix;
    private readonly RedisJsonStore _redis;

    public GameQuestStore(GameQuestTopology topology)
    {
        _redis = new RedisJsonStore(topology.RedisEndpoint);
        _keyPrefix = $"{topology.RedisKeyPrefix}gamequest:";
    }

    public async ValueTask DisposeAsync()
    {
        await _redis.DisposeAsync().ConfigureAwait(false);
    }

    public async ValueTask<GameplayEvent> GetOrAddGameplayEventAsync(
        GameplayEvent candidate,
        CancellationToken cancellationToken)
    {
        var path = Key("gameplay-events");
        var stored = await UpdateAsync(
            path,
            new List<StoredGameplayEvent>(),
            events =>
            {
                var existing = events.FirstOrDefault(e =>
                    string.Equals(e.PlayerId, candidate.PlayerId, StringComparison.Ordinal)
                    && string.Equals(e.IdempotencyKey, candidate.IdempotencyKey, StringComparison.Ordinal));
                if (existing is not null) return existing;

                var contract = ToContract(candidate);
                events.Add(contract);
                return contract;
            },
            cancellationToken);
        return ToDomain(stored);
    }

    public async ValueTask AddUnpublishedKillAsync(
        string playerId,
        int count,
        CancellationToken cancellationToken)
    {
        var path = Key("unpublished-kills");
        await UpdateAsync(
            path,
            new Dictionary<string, int>(StringComparer.Ordinal),
            kills =>
            {
                kills[playerId] = kills.GetValueOrDefault(playerId) + count;
                return 0;
            },
            cancellationToken);
    }

    public async ValueTask<int> GetSnapshotKillCountAsync(
        string playerId,
        CancellationToken cancellationToken)
    {
        var events = await ReadAsync<List<StoredGameplayEvent>>(
            Key("gameplay-events"),
            [],
            cancellationToken);
        var unpublished = await ReadAsync<Dictionary<string, int>>(
            Key("unpublished-kills"),
            new Dictionary<string, int>(StringComparer.Ordinal),
            cancellationToken);
        return events
                   .Where(e => e.PlayerId == playerId && e.EventType == "MonsterKilled")
                   .Sum(e => e.Count)
               + unpublished.GetValueOrDefault(playerId);
    }

    public async ValueTask<GameplaySnapshotData> ReadSnapshotAsync(
        string playerId,
        CancellationToken cancellationToken)
    {
        var events = await ReadAsync<List<StoredGameplayEvent>>(
            Key("gameplay-events"),
            [],
            cancellationToken);
        var unpublishedKills = await ReadAsync<Dictionary<string, int>>(
            Key("unpublished-kills"),
            new Dictionary<string, int>(StringComparer.Ordinal),
            cancellationToken);

        return new GameplaySnapshotData(
            playerId,
            events.Where(e => e.PlayerId == playerId && e.EventType == "MonsterKilled").Sum(e => e.Count)
            + unpublishedKills.GetValueOrDefault(playerId),
            events.Where(e => e.PlayerId == playerId).Select(e => e.CreatedAtUnixMs).DefaultIfEmpty(0).Max()
            + unpublishedKills.GetValueOrDefault(playerId));
    }

    public async ValueTask<QuestProgress[]> ReadProjectionAsync(
        string playerId,
        CancellationToken cancellationToken)
    {
        var all = await ReadAsync<List<QuestProgress>>(
            Key("quest-projection"),
            [],
            cancellationToken);
        return all
            .Where(progress => progress.PlayerId == playerId)
            .OrderBy(progress => progress.QuestId, StringComparer.Ordinal)
            .ToArray();
    }

    public async ValueTask DeleteProjectionAsync(
        string playerId,
        string questId,
        CancellationToken cancellationToken)
    {
        await UpdateAsync(
            Key("quest-projection"),
            new List<QuestProgress>(),
            projection =>
            {
                projection.RemoveAll(progress =>
                    progress.PlayerId == playerId
                    && progress.QuestId == questId);
                return true;
            },
            cancellationToken);
    }

    public async ValueTask<QuestProgress> RebuildProjectionAsync(
        string playerId,
        string questId,
        CancellationToken cancellationToken)
    {
        var events = await ReadQuestEventsAsync(cancellationToken);
        var stream = events
            .Where(e => e.PlayerId == playerId && e.QuestId == questId)
            .OrderBy(e => e.Version)
            .ToArray();
        if (stream.Length == 0)
            throw new InvalidOperationException($"Quest stream was not found for {playerId}/{questId}.");

        var currentCount = 0;
        var requiredCount = 1;
        var status = QuestStatuses.Active;
        string? lastEventId = null;
        long updatedAtUnixMs = 0;
        foreach (var @event in stream)
        {
            var root = @event.Payload;
            if (@event.Type is nameof(QuestProgressedEvent) or nameof(QuestReconciled))
            {
                currentCount = root.GetProperty("CurrentCount").GetInt32();
                if (root.TryGetProperty("RequiredCount", out var required)) requiredCount = required.GetInt32();
            }

            if (@event.Type == nameof(QuestCompletedEvent))
                status = QuestStatuses.Completed;
            else if (@event.Type == nameof(QuestRewardGrantedEvent)) status = QuestStatuses.RewardGranted;

            lastEventId = @event.SourceEventId;
            updatedAtUnixMs = Math.Max(updatedAtUnixMs, @event.CreatedAtUnixMs);
        }

        var rebuilt = new QuestProgress(
            playerId,
            questId,
            status,
            currentCount,
            requiredCount,
            lastEventId,
            stream[^1].Version,
            updatedAtUnixMs);
        await UpdateAsync(
            Key("quest-projection"),
            new List<QuestProgress>(),
            projection =>
            {
                projection.RemoveAll(progress =>
                    progress.PlayerId == playerId
                    && progress.QuestId == questId);
                projection.Add(rebuilt);
                return true;
            },
            cancellationToken);
        return rebuilt;
    }

    public async ValueTask<StoredQuestEvent[]> ReadQuestEventsAsync(CancellationToken cancellationToken)
    {
        var events = await ReadAsync<List<StoredQuestEvent>>(
            Key("quest-events"),
            [],
            cancellationToken);
        return events
            .OrderBy(e => e.PlayerId, StringComparer.Ordinal)
            .ThenBy(e => e.QuestId, StringComparer.Ordinal)
            .ThenBy(e => e.Version)
            .ToArray();
    }

    public async ValueTask<Dictionary<string, int>> ReadOwnerRehydrateEvidenceAsync(CancellationToken cancellationToken)
    {
        return await ReadAsync<Dictionary<string, int>>(
            Key("owner-rehydrates"),
            new Dictionary<string, int>(StringComparer.Ordinal),
            cancellationToken);
    }

    private async ValueTask<TResult> UpdateAsync<T, TResult>(
        string key,
        T fallback,
        Func<T, TResult> update,
        CancellationToken cancellationToken)
    {
        return await _redis.UpdateAsync(key, fallback, update, cancellationToken).ConfigureAwait(false);
    }

    private ValueTask<T> ReadAsync<T>(
        string key,
        T fallback,
        CancellationToken cancellationToken)
    {
        return _redis.ReadAsync(key, fallback, cancellationToken);
    }

    private string Key(string name) => $"{_keyPrefix}{name}";

    private static StoredGameplayEvent ToContract(GameplayEvent gameplayEvent)
    {
        return new StoredGameplayEvent(
            gameplayEvent.EventId,
            gameplayEvent.PlayerId,
            gameplayEvent.IdempotencyKey,
            gameplayEvent.EventType,
            gameplayEvent.Value,
            gameplayEvent.Count,
            gameplayEvent.SourceApi,
            gameplayEvent.CreatedAtUnixMs);
    }

    private static GameplayEvent ToDomain(StoredGameplayEvent contract)
    {
        return new GameplayEvent(
            contract.EventId,
            contract.PlayerId,
            contract.IdempotencyKey,
            contract.EventType,
            contract.Value,
            contract.Count,
            contract.SourceApi,
            contract.CreatedAtUnixMs);
    }

    private sealed record StoredGameplayEvent(
        string EventId,
        string PlayerId,
        string IdempotencyKey,
        string EventType,
        string Value,
        int Count,
        string SourceApi,
        long CreatedAtUnixMs);
}

internal sealed record GameplaySnapshotData(
    string PlayerId,
    int KillCount,
    long SnapshotVersion);
