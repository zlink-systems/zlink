using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;

namespace DeliveryDispatch.Server.CustomerGateway;

internal sealed class CustomerActorAccess(
    IZLinkActorManager actorManager,
    ILogger<CustomerActorAccess> logger)
{
    public ValueTask<ActorRef?> FindAsync(
        string customerId,
        CancellationToken cancellationToken)
    {
        return actorManager.FindAsync(customerId, cancellationToken);
    }

    public async ValueTask<ActorRef> EnsureAsync(
        string customerId,
        CancellationToken cancellationToken)
    {
        var actor = (await actorManager.GetOrCreate(customerId, SampleNames.CustomerActorType)
            .Request(new EnsureCustomerActorReq(customerId)).Async(cancellationToken)) switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Customer Actor creation was rejected.")
        };
        logger.LogInformation(
            "deliverydispatch customer-access: ensured customer={CustomerId}",
            customerId);
        return actor;
    }
}
