using GameQuest.GameApi.Domain;
using GameQuest.Server.Configuration;

namespace GameQuest.GameApi.Application;

internal sealed class GameplayActionService(
    IGameplayEventStore store,
    IGameplayEventOwnerDispatcher ownerDispatcher,
    GameQuestRuntimeConfiguration configuration,
    ILogger<GameplayActionService> logger)
{
    private readonly string _apiName = configuration.InstanceName;

    public async ValueTask<string> KillMonsterAsync(
        string playerId,
        string monsterId,
        string areaId,
        string idempotencyKey,
        CancellationToken cancellationToken)
    {
        var dispatched = await StoreAndDispatchAsync(
            GameplayDomain.CreateMonsterKilled(playerId, monsterId, areaId, idempotencyKey, _apiName),
            cancellationToken);
        return dispatched.EventId;
    }

    public async ValueTask<string> CollectItemAsync(
        string playerId,
        string itemId,
        int count,
        string idempotencyKey,
        CancellationToken cancellationToken)
    {
        var dispatched = await StoreAndDispatchAsync(
            GameplayDomain.CreateItemCollected(playerId, itemId, count, idempotencyKey, _apiName),
            cancellationToken);
        return dispatched.EventId;
    }

    public async ValueTask<string> EnterAreaAsync(
        string playerId,
        string areaId,
        string idempotencyKey,
        CancellationToken cancellationToken)
    {
        var dispatched = await StoreAndDispatchAsync(
            GameplayDomain.CreateAreaEntered(playerId, areaId, idempotencyKey, _apiName),
            cancellationToken);
        return dispatched.EventId;
    }

    private async ValueTask<GameplayEvent> StoreAndDispatchAsync(
        GameplayEvent candidate,
        CancellationToken cancellationToken)
    {
        var stored = await store.GetOrAddGameplayEventAsync(candidate, cancellationToken);
        var routedTo = await ownerDispatcher.DispatchAsync(stored, cancellationToken);
        logger.LogInformation(
            "gamequest api event routed api={Api} player={PlayerId} event={EventId} type={EventType} owner={Owner}",
            _apiName,
            stored.PlayerId,
            stored.EventId,
            stored.EventType,
            routedTo);
        return stored;
    }
}

internal interface IGameplayEventStore
{
    ValueTask<GameplayEvent> GetOrAddGameplayEventAsync(
        GameplayEvent candidate,
        CancellationToken cancellationToken);
}

internal interface IGameplayEventOwnerDispatcher
{
    ValueTask<string> DispatchAsync(
        GameplayEvent gameplayEvent,
        CancellationToken cancellationToken);
}
