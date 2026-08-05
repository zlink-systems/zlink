package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.OrderWorkflowRouter
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RebuildProjectionApiReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RebuildProjectionApiRes

/**
 * Relays projection rebuild to the OrderWorkflow owner; CommerceApi never
 * rebuilds the store itself.
 */
@ZLinkHandlerGroup("commerce")
class RebuildProjectionHandler(
    private val workflows: OrderWorkflowRouter,
) : ZLinkSuspendingRequestHandler<RebuildProjectionApiReq, RebuildProjectionApiRes> {
    override suspend fun handle(
        request: RebuildProjectionApiReq,
        context: ZLinkMessageContext,
    ) = run {
        RebuildProjectionApiRes(workflows.rebuildProjection(request.orderId))
    }
}
