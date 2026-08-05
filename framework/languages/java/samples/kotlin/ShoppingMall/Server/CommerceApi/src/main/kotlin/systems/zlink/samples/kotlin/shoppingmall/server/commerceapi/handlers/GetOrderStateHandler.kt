package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.CommerceStore
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.GetOrderStateReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.GetOrderStateRes

/** Read-only projection lookup. Never advances the workflow or appends events. */
@ZLinkHandlerGroup("commerce")
class GetOrderStateHandler(
    private val store: CommerceStore,
) : ZLinkSuspendingRequestHandler<GetOrderStateReq, GetOrderStateRes> {
    override suspend fun handle(
        request: GetOrderStateReq,
        context: ZLinkMessageContext,
    ): GetOrderStateRes {
        val state = store.findReadModel(request.orderId)
            ?: throw IllegalStateException("Order '${request.orderId}' does not exist.")
        return GetOrderStateRes(state)
    }
}
