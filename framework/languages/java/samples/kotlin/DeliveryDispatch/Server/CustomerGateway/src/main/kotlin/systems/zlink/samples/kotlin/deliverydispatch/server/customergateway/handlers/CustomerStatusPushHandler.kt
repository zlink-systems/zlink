package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.CustomerActorDirectory
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatusChangedRes
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatusChangedReq

@ZLinkHandlerGroup("customer-route")
class CustomerStatusPushHandler(
    private val customers: CustomerActorDirectory,
) : ZLinkSuspendingRequestHandler<DeliveryStatusChangedReq, DeliveryStatusChangedRes> {
    override suspend fun handle(
        request: DeliveryStatusChangedReq,
        context: ZLinkMessageContext,
    ): DeliveryStatusChangedRes {
        customers.push(request)
        return DeliveryStatusChangedRes(request.deliveryId, request.status)
    }
}
