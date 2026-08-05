package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.OfferDeliveryMsg
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.OfferDeliveryNotify

/**
 * A courier. It pushes the offer to whoever holds this courier's stream session and its turn ends
 * there — it does not wait for the answer, and there is nothing left here that could wait (common
 * sample spec section 7.4). The answer arrives later as a `CourierDecisionMsg` from the session,
 * and the only thing the actor has to remember in between is which attempt the offer belonged to,
 * so the decision can be paired with it.
 */
class CourierActor(
    private val id: String,
    private val actorContext: ZLinkActorContext,
) : ZLinkActor {
    private val offeredAttempts = mutableMapOf<String, Int>()

    fun actorId(): String = id

    override fun context(): ZLinkActorContext = actorContext

    /** Pushes the offer and returns. The courier takes as long as it takes. */
    fun offer(offer: OfferDeliveryMsg) {
        synchronized(offeredAttempts) {
            offeredAttempts[offer.deliveryId] = offer.attempt
        }
        actorContext.boundSession()
            .send(
                OfferDeliveryNotify(
                    courierId = offer.courierId,
                    deliveryId = offer.deliveryId,
                    pickupAddress = offer.pickupAddress,
                    dropoffAddress = offer.dropoffAddress,
                ),
            )
            .submit()
    }

    /**
     * The attempt the courier is answering, or null when this actor knows of no such offer — it was
     * already answered, or this actor was never offered it.
     */
    fun takeOfferedAttempt(deliveryId: String): Int? =
        synchronized(offeredAttempts) { offeredAttempts.remove(deliveryId) }
}
