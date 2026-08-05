package systems.zlink.samples.kotlin.deliverydispatch.server.tracking.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.DeliveryEvidenceStore
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.ServerAssertionReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.ServerAssertionRes

@ZLinkHandlerGroup("tracking")
class ServerAssertionHandler(
    private val evidenceStore: DeliveryEvidenceStore,
) : ZLinkSuspendingRequestHandler<ServerAssertionReq, ServerAssertionRes> {
    override suspend fun handle(
        request: ServerAssertionReq,
        context: ZLinkMessageContext,
    ): ServerAssertionRes =
        evidenceStore.assertSequences(
            request.successfulDeliveryId,
            request.reassignedDeliveryId,
        )
}
