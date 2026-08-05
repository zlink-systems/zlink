package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.StartOrderUseCase
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.PrepareInventoryReservedApiReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.PrepareInventoryReservedApiRes

/**
 * Self-check hook for crash-recovery coverage. CommerceApi still validates the
 * public start request and builds the workflow command before relaying it.
 */
@ZLinkHandlerGroup("commerce")
class PrepareInventoryReservedHandler(
    private val useCase: StartOrderUseCase,
) : ZLinkSuspendingRequestHandler<PrepareInventoryReservedApiReq, PrepareInventoryReservedApiRes> {
    override suspend fun handle(
        request: PrepareInventoryReservedApiReq,
        context: ZLinkMessageContext,
    ): PrepareInventoryReservedApiRes =
        PrepareInventoryReservedApiRes(useCase.prepareInventoryReserved(request.request))
}
