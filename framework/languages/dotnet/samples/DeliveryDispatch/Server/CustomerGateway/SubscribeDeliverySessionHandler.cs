using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Streams;

namespace DeliveryDispatch.Server.CustomerGateway;

internal sealed class SubscribeDeliverySessionHandler(
    CustomerActorAccess actors,
    CustomerActorDirectory directory,
    ILogger<SubscribeDeliverySessionHandler> logger)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, SubscribeDeliveryReq>
{
    private const string CustomerId = "customer-1";

    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        SubscribeDeliveryReq request,
        CancellationToken cancellationToken)
    {
        var actor = await actors.FindAsync(CustomerId, cancellationToken)
                    ?? await actors.EnsureAsync(CustomerId, cancellationToken);

        var alreadyBound = context.Actors.Find(CustomerId) is not null;
        _ = await context.Actors.BindOrGetAsync(
            actor,
            cancellationToken);
        if (!alreadyBound)
        {
            logger.LogInformation(
                "deliverydispatch-customer bound customer={CustomerId}",
                CustomerId);
        }

        directory.Subscribe(CustomerId, request.DeliveryId);
        logger.LogInformation(
            "deliverydispatch customer-session: subscribed customer={CustomerId} delivery={DeliveryId}",
            CustomerId,
            request.DeliveryId);
        await context.Client.Reply(new SubscribeDeliveryRes(request.DeliveryId))
            .Async(cancellationToken);
    }
}
