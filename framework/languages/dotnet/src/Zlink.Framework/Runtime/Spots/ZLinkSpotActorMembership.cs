namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActorMembership
{
    private readonly Dictionary<string, IZLinkActor> _actorsById = new(StringComparer.Ordinal);
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
        lock (_gate)
        {
            if (_actorsById.TryGetValue(actor.Context.ActorId, out var existing)
                && !ReferenceEquals(existing, actor))
                throw new InvalidOperationException(
                    $"SPOT already has an actor with id '{actor.Context.ActorId}'.");

            _actorsById[actor.Context.ActorId] = actor;
        }
    }

    public bool TryGetActor(string actorId, out IZLinkActor? actor)
    {
        lock (_gate)
        {
            return _actorsById.TryGetValue(actorId, out actor);
        }
    }

    public void RemoveIfCurrent(IZLinkActor actor)
    {
        lock (_gate)
        {
            if (_actorsById.TryGetValue(actor.Context.ActorId, out var existing)
                && ReferenceEquals(existing, actor))
                _actorsById.Remove(actor.Context.ActorId);
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
