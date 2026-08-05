package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.handlers

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.spots.ZLinkSpotRequestHandler
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.OrderWorkflowSpot
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.OrderWorkflowService
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.WorkflowContinuationQueue
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderWorkflowReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderWorkflowRes

class StartOrderWorkflowHandler(
    private val workflow: OrderWorkflowService,
    private val continuations: WorkflowContinuationQueue,
) : ZLinkSpotRequestHandler<OrderWorkflowSpot, StartOrderWorkflowReq, StartOrderWorkflowRes> {
    override fun handle(
        spot: OrderWorkflowSpot,
        request: StartOrderWorkflowReq,
    ): CompletionStage<StartOrderWorkflowRes> {
        spot.requireOrder(request.orderId)
        val state = workflow.start(request)
        System.err.println("shoppingmall order: started order=${request.orderId} status=${state.status}")
        continuations.enqueue(request.orderId)
        return CompletableFuture.completedFuture(StartOrderWorkflowRes(state))
    }
}
