using Zlink.Framework.Contracts.Handlers;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Spots;

namespace DeliveryDispatch.Server.CourierActorNode.Spots.EntrySpot.Handlers;

/// <summary>The offer, in the courier actor's own turn: push it to the bound session and
/// return (common sample spec §7.4).</summary>
internal sealed class OfferDeliveryActorHandler(ILogger<OfferDeliveryActorHandler> logger)
    : IZLinkEntrySpotActorSendHandler<CourierEntrySpot, CourierActor, OfferDeliveryMsg>
{
    public async ValueTask HandleAsync(
        CourierEntrySpot entrySpot,
        CourierActor actor,
        IZLinkMessageContext context,
        OfferDeliveryMsg message,
        CancellationToken cancellationToken)
    {
        await actor.OfferAsync(message, cancellationToken);
        logger.LogInformation(
            "deliverydispatch courier-actor: offer pushed delivery={DeliveryId} courier={CourierId} attempt={Attempt}",
            message.DeliveryId,
            actor.ActorId,
            message.Attempt);
    }
}
