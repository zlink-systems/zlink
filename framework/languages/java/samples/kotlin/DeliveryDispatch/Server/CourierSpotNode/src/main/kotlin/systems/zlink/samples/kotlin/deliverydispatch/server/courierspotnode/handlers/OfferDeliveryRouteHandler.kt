package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.handlers

import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotPacketHandler
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.OfferDeliveryMsg

/**
 * The offer arrives as a one-way send, is handed to the courier actor, and this handler returns —
 * the spot's serial queue is given straight back, so it is not held for the length of a courier's
 * reaction time. The node does not time the offer either: that deadline belongs to the dispatch
 * worker (common sample spec section 7.4).
 */
class OfferDeliveryRouteHandler(
    private val actors: ZLinkActorManager,
    private val actorClient: ZLinkActorClient,
) : ZLinkSuspendingSpotPacketHandler<CourierEntrySpot, OfferDeliveryMsg> {
    override suspend fun handle(spot: CourierEntrySpot, message: OfferDeliveryMsg) {
        val actorRef =
            actors.find(message.courierId).await().orElseThrow {
                IllegalStateException("Courier actor is not bound: ${message.courierId}")
            }
        actorClient.kotlin().sendToActor(actorRef.actorId, message).await()
    }
}
