package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.StartOrderUseCase
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderRes

@ZLinkHandlerGroup("commerce")
class StartOrderHandler(
    private val useCase: StartOrderUseCase,
) : ZLinkSuspendingRequestHandler<StartOrderReq, StartOrderRes> {
    override suspend fun handle(
        request: StartOrderReq,
        context: ZLinkMessageContext,
    ) = run {
        useCase.execute(request)
    }
}
