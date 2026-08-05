using System.Collections.Concurrent;
using SpotService.Shared;
using Zlink.Framework.Contracts.Actors;

namespace SpotService.Server.Gateway;

internal sealed class ActorCreationCoordinator(IZLinkActorManager actors)
{
    private readonly ConcurrentDictionary<string, Task<ZLinkActorCreateResult>> _operations = new(
        StringComparer.Ordinal);

    public void Start(string actorId)
    {
        _operations.GetOrAdd(actorId, _ => CreateAsync(actorId));
    }

    public async Task<ZLinkActorCreateResult> CompleteAsync(string actorId)
    {
        if (!_operations.TryGetValue(actorId, out var operation))
            throw new InvalidOperationException(
                $"Actor creation operation '{actorId}' was not started.");

        var result = await operation;
        _operations.TryRemove(actorId, out _);
        return result;
    }

    private async Task<ZLinkActorCreateResult> CreateAsync(string actorId)
    {
        return await actors
            .GetOrCreate(actorId, SpotServiceNames.ActorType)
            .InMesh(SpotServiceNames.SpotChannel)
            .Request(new ScenarioActorCreateReq("sm-b11"))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async();
    }
}
