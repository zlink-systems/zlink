package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.handlers

import java.util.concurrent.CompletionStage
import systems.zlink.framework.spots.ZLinkSpotPacketHandler
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.OrderWorkflowService
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.OrderWorkflowSpot
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RunOrderWorkflowMsg

class RunOrderWorkflowHandler(private val workflow: OrderWorkflowService) :
    ZLinkSpotPacketHandler<OrderWorkflowSpot, RunOrderWorkflowMsg> {
    override fun handle(
        spot: OrderWorkflowSpot,
        message: RunOrderWorkflowMsg,
    ): CompletionStage<Void> {
        spot.requireOrder(message.orderId)
        return spot.closeIfTerminal(workflow.continueWorkflow(spot, message.orderId))
    }
}
