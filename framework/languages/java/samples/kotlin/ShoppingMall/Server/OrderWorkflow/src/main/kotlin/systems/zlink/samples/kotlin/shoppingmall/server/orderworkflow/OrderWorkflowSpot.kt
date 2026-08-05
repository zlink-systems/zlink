package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow

import systems.zlink.framework.spots.ZLinkInstanceSpot
import systems.zlink.framework.spots.ZLinkInstanceSpotContext
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.handlers.ContinueOrderWorkflowHandler
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.handlers.PrepareInventoryReservedHandler
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.handlers.RebuildOrderProjectionHandler
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.handlers.StartOrderWorkflowHandler

class OrderWorkflowSpot(
    private val instanceContext: ZLinkInstanceSpotContext,
) : ZLinkInstanceSpot {
    override fun context(): ZLinkInstanceSpotContext = instanceContext

    fun requireOrder(orderId: String) {
        require(orderId == instanceContext.spotId()) {
            "request order does not match workflow Spot: $orderId"
        }
    }
}
