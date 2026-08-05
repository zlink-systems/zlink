package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.handlers

import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.spots.ZLinkSpotPacketHandler
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.OfferDeliveryMsg

/**
 * The offer arrives as a one-way send, is handed to the courier actor, and this handler returns —
 * the spot's serial queue is given straight back, so it is not held for the length of a courier's
 * reaction time. The node does not time the offer either: that deadline belongs to the dispatch
 * worker (common sample spec section 7.4).
 *
 * The suspending packet handler is bound to `ZLinkSpot`, and this one hangs off the entry spot, so
 * it implements the framework interface directly.
 */
class OfferDeliveryRouteHandler(
    private val actors: ZLinkActorManager,
    private val actorClient: ZLinkActorClient,
) : ZLinkSpotPacketHandler<CourierEntrySpot, OfferDeliveryMsg> {
    override fun handle(
        spot: CourierEntrySpot,
        message: OfferDeliveryMsg,
    ): CompletionStage<Void> =
        actors.find(message.courierId).thenAccept { found ->
            val actorRef = found.orElseThrow {
                IllegalStateException("Courier actor is not bound: ${message.courierId}")
            }
            actorClient.sendToActor(actorRef.actorId, message).submit()
        }
}
