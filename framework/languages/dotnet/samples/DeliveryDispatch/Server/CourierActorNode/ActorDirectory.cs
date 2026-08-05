namespace DeliveryDispatch.Server.CourierActorNode;

internal sealed class ActorDirectory
{
    private readonly object _gate = new();
    private readonly Dictionary<string, CourierActor> _actors = new(StringComparer.Ordinal);

    public void Register(CourierActor actor)
    {
        lock (_gate)
        {
            _actors[actor.ActorId] = actor;
        }
    }

    public CourierActor Require(string courierId)
    {
        lock (_gate)
        {
            if (_actors.TryGetValue(courierId, out var actor))
            {
                return actor;
            }
        }

        throw new InvalidOperationException($"Courier actor is not available on this node: {courierId}");
    }
}
