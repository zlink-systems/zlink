using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActorMembership
{
    private readonly Dictionary<ZLinkActorId, IZLinkActor> _actorsById = [];
    private readonly object _gate = new();

    public int Count
    {
        get
        {
            lock (_gate)
            {
                return _actorsById.Count;
            }
        }
    }

    public void Add(IZLinkActor actor)
    {
        var actorId = ZLinkActorId.FromBoundary(
            actor.Context.ActorId,
            nameof(actor));
        lock (_gate)
        {
            if (_actorsById.TryGetValue(actorId, out var existing)
                && !ReferenceEquals(existing, actor))
                throw new InvalidOperationException(
                    $"SPOT already has an actor with id '{actor.Context.ActorId}'.");

            _actorsById[actorId] = actor;
        }
    }

    internal bool TryGetActor(ZLinkActorId actorId, out IZLinkActor? actor)
    {
        lock (_gate)
        {
            return _actorsById.TryGetValue(actorId, out actor);
        }
    }

    public void RemoveIfCurrent(IZLinkActor actor)
    {
        var actorId = ZLinkActorId.FromBoundary(
            actor.Context.ActorId,
            nameof(actor));
        lock (_gate)
        {
            if (_actorsById.TryGetValue(actorId, out var existing)
                && ReferenceEquals(existing, actor))
                _actorsById.Remove(actorId);
        }
    }

    public IReadOnlyList<IZLinkActor> Snapshot()
    {
        lock (_gate)
            return _actorsById.Values
                .OrderBy(static actor => actor.Context.ActorId, StringComparer.Ordinal)
                .ToArray();
    }
}
