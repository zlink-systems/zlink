package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.handlers

import java.util.concurrent.CompletionStage
import systems.zlink.framework.spots.ZLinkSpotRequestHandler
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.OrderWorkflowService
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.OrderWorkflowSpot
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ContinueOrderWorkflowReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ContinueOrderWorkflowRes

class ContinueOrderWorkflowHandler(private val workflow: OrderWorkflowService) :
    ZLinkSpotRequestHandler<OrderWorkflowSpot, ContinueOrderWorkflowReq, ContinueOrderWorkflowRes> {
    override fun handle(
        spot: OrderWorkflowSpot,
        request: ContinueOrderWorkflowReq,
    ): CompletionStage<ContinueOrderWorkflowRes> {
        spot.requireOrder(request.orderId)
        val state = workflow.continueWorkflow(spot, request.orderId)
        return spot.closeIfTerminal(state).thenApply {
            ContinueOrderWorkflowRes(state, spot.context().objectGeneration())
        }
    }
}
