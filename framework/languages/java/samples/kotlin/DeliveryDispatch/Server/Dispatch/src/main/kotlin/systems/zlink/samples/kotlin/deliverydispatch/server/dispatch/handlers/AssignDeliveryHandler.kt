package systems.zlink.samples.kotlin.deliverydispatch.server.dispatch.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.server.dispatch.DispatchWorkQueue
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.AssignDeliveryMsg

/**
 * The one-way admission of a new delivery onto the dispatch channel (common sample spec section
 * 7.1). The HTTP edge never calls into worker state directly — it sends this message, and this
 * handler is the only path that hands it to the in-process work queue.
 */
@ZLinkHandlerGroup(SampleNames.DispatchChannel)
class AssignDeliveryHandler(
    private val queue: DispatchWorkQueue,
) : ZLinkSuspendingSendHandler<AssignDeliveryMsg> {
    override suspend fun handle(message: AssignDeliveryMsg, context: ZLinkMessageContext) {
        queue.enqueue(message)
        println(
            "deliverydispatch dispatch-channel: enqueued delivery=${message.deliveryId} " +
                "customer=${message.customerId}",
        )
    }
}
