package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.handlers

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.spots.ZLinkSpotRequestHandler
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.OrderWorkflowSpot
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.OrderWorkflowService
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.PrepareInventoryReservedReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.PrepareInventoryReservedRes

/**
 * Self-check hook that stops after the inventory reservation checkpoint so the
 * client can prove explicit recovery resumes at the next event-stream step.
 */
class PrepareInventoryReservedHandler(
    private val workflow: OrderWorkflowService,
) : ZLinkSpotRequestHandler<OrderWorkflowSpot, PrepareInventoryReservedReq, PrepareInventoryReservedRes> {
    override fun handle(
        spot: OrderWorkflowSpot,
        request: PrepareInventoryReservedReq,
    ): CompletionStage<PrepareInventoryReservedRes> {
        spot.requireOrder(request.command.orderId)
        return CompletableFuture.completedFuture(
            PrepareInventoryReservedRes(workflow.prepareInventoryReserved(request.command)),
        )
    }
}
