package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.handlers

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.spots.ZLinkSpotRequestHandler
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.OrderWorkflowSpot
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.OrderWorkflowService
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RebuildOrderProjectionReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RebuildOrderProjectionRes

class RebuildOrderProjectionHandler(
    private val workflow: OrderWorkflowService,
) : ZLinkSpotRequestHandler<OrderWorkflowSpot, RebuildOrderProjectionReq, RebuildOrderProjectionRes> {
    override fun handle(
        spot: OrderWorkflowSpot,
        request: RebuildOrderProjectionReq,
    ): CompletionStage<RebuildOrderProjectionRes> {
        spot.requireOrder(request.orderId)
        val state = workflow.rebuildProjection(request.orderId)
        System.err.println("shoppingmall projection: rebuilt order=${request.orderId} status=${state.status}")
        return CompletableFuture.completedFuture(RebuildOrderProjectionRes(state))
    }
}
