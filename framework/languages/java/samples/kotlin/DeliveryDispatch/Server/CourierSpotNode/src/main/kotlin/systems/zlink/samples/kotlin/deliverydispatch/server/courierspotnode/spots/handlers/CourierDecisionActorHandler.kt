package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots.handlers

import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorSendHandler
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.CourierActor
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.CourierDecisionMsg
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.OfferDeliveryResultMsg

/**
 * The courier's decision, sent back to dispatch one-way. The node neither judges it nor times it —
 * it carries the attempt the offer was made under, and dispatch decides whether that attempt is
 * still the current one (common sample spec section 7.4).
 */
class CourierDecisionActorHandler(
    private val channels: ZLinkClient,
) : ZLinkSuspendingEntrySpotActorSendHandler<
    CourierEntrySpot,
    CourierActor,
    CourierDecisionMsg,
    > {
    override suspend fun handle(
        entrySpot: CourierEntrySpot,
        actor: CourierActor,
        context: ZLinkMessageContext,
        message: CourierDecisionMsg,
    ) {
        val attempt = actor.takeOfferedAttempt(message.deliveryId)
        if (attempt == null) {
            System.err.println(
                "deliverydispatch courier-actor: decision for an unknown offer " +
                    "delivery=${message.deliveryId} courier=${actor.actorId()}",
            )
            return
        }

        channels
            .sendToChannel(
                SampleNames.DispatchChannel,
                OfferDeliveryResultMsg(
                    deliveryId = message.deliveryId,
                    courierId = message.courierId,
                    attempt = attempt,
                    accepted = message.accepted,
                    reason = message.reason,
                ),
            )
            .submit()
        println(
            "deliverydispatch courier-actor: decision delivery=${message.deliveryId} " +
                "courier=${actor.actorId()} attempt=$attempt accepted=${message.accepted}",
        )
    }
}
