package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow

import java.util.concurrent.CompletionStage
import systems.zlink.framework.spots.ZLinkInstanceSpot
import systems.zlink.framework.spots.ZLinkInstanceSpotContext
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderState
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderStatuses

class OrderWorkflowSpot(private val instanceContext: ZLinkInstanceSpotContext) : ZLinkInstanceSpot {
    override fun context(): ZLinkInstanceSpotContext = instanceContext

    fun requireOrder(orderId: String) {
        require(orderId == instanceContext.spotId()) {
            "request order does not match workflow Spot: $orderId"
        }
    }

    // --8<-- [start:doc-sm-close-terminal]
    fun closeIfTerminal(state: OrderState): CompletionStage<Void> =
        if (isTerminal(state)) {
            instanceContext.close()
            java.util.concurrent.CompletableFuture.completedFuture<Void>(null)
        } else {
            java.util.concurrent.CompletableFuture.completedFuture<Void>(null)
        }

    // --8<-- [end:doc-sm-close-terminal]

    fun isTerminal(state: OrderState): Boolean =
        state.status == OrderStatuses.Confirmed || state.status == OrderStatuses.Failed
}
