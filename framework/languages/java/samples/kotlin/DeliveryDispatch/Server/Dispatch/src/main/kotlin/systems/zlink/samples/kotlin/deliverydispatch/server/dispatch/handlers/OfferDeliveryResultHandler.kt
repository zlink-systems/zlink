package systems.zlink.samples.kotlin.deliverydispatch.server.dispatch.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.server.dispatch.DeliveryOfferStore
import systems.zlink.samples.kotlin.deliverydispatch.server.dispatch.DispatchWorker
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.OfferDeliveryResultMsg

/**
 * The courier's decision, arriving as its own inbound message rather than as the reply to a request
 * nobody could have made. A decision naming an attempt other than the one on record came back after
 * the offer had already been reassigned, and is dropped — that check is what makes it safe never to
 * wait (common sample spec section 7.4).
 */
@ZLinkHandlerGroup(SampleNames.DispatchChannel)
class OfferDeliveryResultHandler(
    private val offers: DeliveryOfferStore,
    private val worker: DispatchWorker,
) : ZLinkSuspendingSendHandler<OfferDeliveryResultMsg> {
    override suspend fun handle(message: OfferDeliveryResultMsg, context: ZLinkMessageContext) {
        val offer = offers.settle(message.deliveryId, message.attempt)
        if (offer == null) {
            println(
                "deliverydispatch dispatch: dropped a late decision " +
                    "delivery=${message.deliveryId} attempt=${message.attempt}",
            )
            return
        }

        println(
            "deliverydispatch dispatch: decision delivery=${message.deliveryId} " +
                "courier=${message.courierId} attempt=${message.attempt} accepted=${message.accepted}",
        )
        worker.settle(offer, message.accepted, message.reason)
    }
}
