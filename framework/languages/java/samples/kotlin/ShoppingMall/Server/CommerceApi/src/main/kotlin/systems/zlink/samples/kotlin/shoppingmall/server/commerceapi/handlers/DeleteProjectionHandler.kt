package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.CommerceStore
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.DeleteProjectionReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.DeleteProjectionRes

@ZLinkHandlerGroup("commerce")
class DeleteProjectionHandler(
    private val store: CommerceStore,
) : ZLinkSuspendingRequestHandler<DeleteProjectionReq, DeleteProjectionRes> {
    override suspend fun handle(
        request: DeleteProjectionReq,
        context: ZLinkMessageContext,
    ): DeleteProjectionRes = DeleteProjectionRes(store.deleteReadModel(request.orderId))
}
