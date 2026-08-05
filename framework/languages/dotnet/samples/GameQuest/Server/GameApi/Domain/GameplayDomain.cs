namespace GameQuest.GameApi.Domain;

internal sealed record GameplayEvent(
    string EventId,
    string PlayerId,
    string IdempotencyKey,
    string EventType,
    string Value,
    int Count,
    string SourceApi,
    long CreatedAtUnixMs);

internal static class GameplayDomain
{
    public static GameplayEvent CreateMonsterKilled(
        string playerId,
        string monsterId,
        string areaId,
        string idempotencyKey,
        string sourceApi)
    {
        return Create(playerId, idempotencyKey, "MonsterKilled", monsterId, 1, sourceApi);
    }

    public static GameplayEvent CreateItemCollected(
        string playerId,
        string itemId,
        int count,
        string idempotencyKey,
        string sourceApi)
    {
        return Create(playerId, idempotencyKey, "ItemCollected", itemId, count, sourceApi);
    }

    public static GameplayEvent CreateAreaEntered(
        string playerId,
        string areaId,
        string idempotencyKey,
        string sourceApi)
    {
        return Create(playerId, idempotencyKey, "AreaEntered", areaId, 1, sourceApi);
    }

    private static GameplayEvent Create(
        string playerId,
        string idempotencyKey,
        string eventType,
        string value,
        int count,
        string sourceApi)
    {
        if (string.IsNullOrWhiteSpace(playerId)) throw new InvalidOperationException("Player id is required.");

        if (string.IsNullOrWhiteSpace(idempotencyKey))
            throw new InvalidOperationException("Idempotency key is required.");

        if (count <= 0) throw new InvalidOperationException("Count must be positive.");

        return new GameplayEvent(
            $"{playerId}-{idempotencyKey}",
            playerId,
            idempotencyKey,
            eventType,
            value,
            count,
            sourceApi,
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
    }
}
