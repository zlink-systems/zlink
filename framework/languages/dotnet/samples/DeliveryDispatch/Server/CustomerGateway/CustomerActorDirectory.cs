using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;

namespace DeliveryDispatch.Server.CustomerGateway;

internal sealed class CustomerActorDirectory(ILogger<CustomerActorDirectory> logger)
{
    private readonly object _gate = new();
    private readonly Dictionary<string, CustomerActor> _actors = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _deliveryCustomers = new(StringComparer.Ordinal);

    public void Register(CustomerActor actor)
    {
        lock (_gate)
        {
            _actors[actor.ActorId] = actor;
        }
    }

    public void Subscribe(string customerId, string deliveryId)
    {
        lock (_gate)
        {
            _deliveryCustomers[deliveryId] = customerId;
        }
    }

    public ValueTask PushAsync(
        DeliveryStatusUpdatedMsg status,
        CancellationToken cancellationToken)
    {
        CustomerActor? actor = null;
        lock (_gate)
        {
            if (_deliveryCustomers.TryGetValue(status.DeliveryId, out var customerId))
            {
                actor = _actors.TryGetValue(customerId, out var value) ? value : null;
            }
        }

        if (actor is null)
        {
            logger.LogWarning(
                "deliverydispatch customer-gateway: no bound customer for delivery={DeliveryId}",
                status.DeliveryId);
            return ValueTask.CompletedTask;
        }

        return actor.PushStatusAsync(status, cancellationToken);
    }
}
