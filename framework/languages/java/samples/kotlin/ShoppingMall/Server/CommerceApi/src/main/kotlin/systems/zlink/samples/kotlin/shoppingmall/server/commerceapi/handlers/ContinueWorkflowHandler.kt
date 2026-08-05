package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.OrderWorkflowRouter
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ContinueOrderWorkflowReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ContinueOrderWorkflowRes

/** Self-check hook that relays explicit recovery to the workflow owner. */
@ZLinkHandlerGroup("commerce")
class ContinueWorkflowHandler(
    private val workflows: OrderWorkflowRouter,
) : ZLinkSuspendingRequestHandler<ContinueOrderWorkflowReq, ContinueOrderWorkflowRes> {
    override suspend fun handle(
        request: ContinueOrderWorkflowReq,
        context: ZLinkMessageContext,
    ): ContinueOrderWorkflowRes =
        ContinueOrderWorkflowRes(workflows.continueWorkflow(request.orderId))
}
