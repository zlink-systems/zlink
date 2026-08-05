package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots.handlers

import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorSendHandler
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.CourierActor
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.OfferDeliveryMsg

/**
 * The offer, in the courier actor's own turn: push it to the bound session and return (common
 * sample spec section 7.4).
 */
class OfferDeliveryActorHandler : ZLinkSuspendingEntrySpotActorSendHandler<
    CourierEntrySpot,
    CourierActor,
    OfferDeliveryMsg,
    > {
    override suspend fun handle(
        entrySpot: CourierEntrySpot,
        actor: CourierActor,
        context: ZLinkMessageContext,
        message: OfferDeliveryMsg,
    ) {
        actor.offer(message)
        println(
            "deliverydispatch courier-actor: offer pushed delivery=${message.deliveryId} " +
                "courier=${actor.actorId()} attempt=${message.attempt}",
        )
    }
}
