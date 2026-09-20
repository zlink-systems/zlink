using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace DeliveryDispatch.Server.CustomerGateway.Spots.EntrySpot.Handlers;

using CustomerStatusPacketHandler = IZLinkSpotPacketHandler<
    CustomerEntrySpot,
    DeliveryStatusUpdatedMsg
>;

internal sealed class DeliveryStatusUpdatedHandler(ILogger<DeliveryStatusUpdatedHandler> logger)
    : IZLinkEntrySpotActorSendHandler<CustomerEntrySpot, CustomerActor, DeliveryStatusUpdatedMsg>
{
    // --8<-- [start:doc-dd-customer-push]
    public async ValueTask HandleAsync(
        CustomerEntrySpot spot,
        CustomerActor actor,
        IZLinkMessageContext context,
        DeliveryStatusUpdatedMsg message,
        CancellationToken cancellationToken
    )
    {
        logger.LogInformation(
            "deliverydispatch customer-entry: status delivery={DeliveryId} customer={CustomerId} actor={ActorId} status={Status}",
            message.DeliveryId,
            message.CustomerId,
            actor.ActorId,
            message.Status
        );
        await actor.PushStatusAsync(message, cancellationToken);
        logger.LogInformation(
            "deliverydispatch-customer pushed status={Status} delivery={DeliveryId}",
            message.Status,
            message.DeliveryId
        );
    }
    // --8<-- [end:doc-dd-customer-push]
}
