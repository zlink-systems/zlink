using GameQuest.QuestMission.Application;
using GameQuest.QuestMission.Domain;
using GameQuest.Server.Configuration;
using GameQuest.Shared;

namespace GameQuest.QuestMission.Infrastructure.Store;

internal sealed class QuestStore : IQuestStore, IAsyncDisposable
{
    private readonly string _keyPrefix;
    private readonly RedisJsonStore _redis;

    public QuestStore(GameQuestTopology topology)
    {
        _redis = new RedisJsonStore(topology.RedisEndpoint);
        _keyPrefix = $"{topology.RedisKeyPrefix}gamequest:";
    }

    public async ValueTask DisposeAsync()
    {
        await _redis.DisposeAsync().ConfigureAwait(false);
    }

    public async ValueTask<QuestDomainEvent[]> ReadQuestStreamAsync(
        string playerId,
        string questId,
        CancellationToken cancellationToken)
    {
        var events = await ReadDomainEventsAsync(cancellationToken);
        return events
            .Where(e => e.PlayerId == playerId && e.QuestId == questId)
            .OrderBy(e => e.Version)
            .ToArray();
    }

    public async ValueTask<QuestProgressState[]> ReadProjectionAsync(
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
            .Select(QuestContractMapper.ToDomain)
            .ToArray();
    }

    public async ValueTask<bool> AppendAndProjectAsync(
        QuestProgressState progress,
        IReadOnlyList<QuestDomainEvent> events,
        CancellationToken cancellationToken)
    {
        var contracts = events.Select(QuestContractMapper.ToContract).ToArray();
        var appended = false;
        await UpdateAsync(
            Key("quest-events"),
            new List<StoredQuestEvent>(),
            stored =>
            {
                if (stored.Any(existing =>
                        existing.PlayerId == progress.PlayerId
                        && existing.QuestId == progress.QuestId
                        && existing.SourceEventId == progress.LastSourceEventId))
                    return;

                var nextVersion = stored
                    .Where(existing => existing.PlayerId == progress.PlayerId && existing.QuestId == progress.QuestId)
                    .Select(existing => existing.Version)
                    .DefaultIfEmpty(0)
                    .Max() + 1;
                if (contracts.Length == 0
                    || contracts[0].Version != nextVersion
                    || contracts.Where((item, index) => item.Version != nextVersion + index).Any()
                    || contracts.Any(item => stored.Any(existing => existing.EventId == item.EventId)))
                    return;

                stored.AddRange(contracts);
                appended = true;
            },
            cancellationToken);

        if (!appended) return false;

        await UpdateAsync(
            Key("quest-projection"),
            new List<QuestProgress>(),
            projection =>
            {
                projection.RemoveAll(p => p.PlayerId == progress.PlayerId && p.QuestId == progress.QuestId);
                projection.Add(QuestContractMapper.ToContract(progress));
            },
            cancellationToken);
        return true;
    }

    public async ValueTask<StoredQuestEvent[]> ReadStoredEventsAsync(CancellationToken cancellationToken)
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

    private async ValueTask<QuestDomainEvent[]> ReadDomainEventsAsync(CancellationToken cancellationToken)
    {
        return (await ReadStoredEventsAsync(cancellationToken))
            .Select(QuestContractMapper.ToDomain)
            .ToArray();
    }

    public async ValueTask RecordOwnerRehydratedAsync(
        string playerId,
        CancellationToken cancellationToken)
    {
        await UpdateAsync(
            Key("owner-rehydrates"),
            new Dictionary<string, int>(StringComparer.Ordinal),
            evidence =>
            {
                evidence[playerId] = evidence.GetValueOrDefault(playerId) + 1;
            },
            cancellationToken);
    }

    public async ValueTask<Dictionary<string, int>> ReadOwnerRehydrateCountsAsync(CancellationToken cancellationToken)
    {
        return await ReadAsync(
            Key("owner-rehydrates"),
            new Dictionary<string, int>(StringComparer.Ordinal),
            cancellationToken);
    }

    private async ValueTask UpdateAsync<T>(
        string key,
        T fallback,
        Action<T> update,
        CancellationToken cancellationToken)
    {
        await _redis.UpdateAsync(key, fallback, update, cancellationToken).ConfigureAwait(false);
    }

    private ValueTask<T> ReadAsync<T>(string key, T fallback, CancellationToken cancellationToken)
    {
        return _redis.ReadAsync(key, fallback, cancellationToken);
    }

    private string Key(string name) => $"{_keyPrefix}{name}";
}
