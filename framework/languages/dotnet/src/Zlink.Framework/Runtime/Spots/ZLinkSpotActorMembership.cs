using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActorMembership
{
    private readonly ConcurrentDictionary<ZLinkActorId, IZLinkActor> _actorsById = new();

    public int Count
    {
        get => _actorsById.Count;
    }

    public void Add(IZLinkActor actor)
    {
        var actorId = ZLinkActorId.FromBoundary(
            actor.Context.ActorId,
            nameof(actor));
        var existing = _actorsById.GetOrAdd(actorId, actor);
        if (!ReferenceEquals(existing, actor))
            throw new InvalidOperationException(
                $"SPOT already has an actor with id '{actor.Context.ActorId}'.");
    }

    internal bool TryGetActor(ZLinkActorId actorId, out IZLinkActor? actor)
    {
        return _actorsById.TryGetValue(actorId, out actor);
    }

    public bool RemoveIfCurrent(IZLinkActor actor)
    {
        var actorId = ZLinkActorId.FromBoundary(
            actor.Context.ActorId,
            nameof(actor));
        if (_actorsById.TryGetValue(actorId, out var existing)
            && ReferenceEquals(existing, actor))
            return ((ICollection<KeyValuePair<ZLinkActorId, IZLinkActor>>)_actorsById).Remove(
                new KeyValuePair<ZLinkActorId, IZLinkActor>(actorId, actor));
        return false;
    }

    public IReadOnlyList<IZLinkActor> Snapshot()
    {
        return _actorsById.Values
            .OrderBy(static actor => actor.Context.ActorId, StringComparer.Ordinal)
            .ToArray();
    }
}
