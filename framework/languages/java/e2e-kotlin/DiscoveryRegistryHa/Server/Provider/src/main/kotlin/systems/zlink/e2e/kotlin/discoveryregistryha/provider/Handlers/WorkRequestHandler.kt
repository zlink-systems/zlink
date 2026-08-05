package systems.zlink.e2e.kotlin.discoveryregistryha.provider.Handlers

import systems.zlink.e2e.kotlin.discoveryregistryha.Contracts
import systems.zlink.e2e.kotlin.discoveryregistryha.provider.Support.ProviderEvidenceStore
import systems.zlink.framework.channels.ZLinkRequestContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
class WorkRequestHandler(
    private val state: ProviderEvidenceStore,
) : ZLinkSuspendingRequestHandler<Contracts.WorkReq, Contracts.WorkRes> {
    override suspend fun handle(
        request: Contracts.WorkReq,
        context: ZLinkRequestContext,
    ): Contracts.WorkRes =
        Contracts.WorkRes("work:${request.value}", state.providerRid)
}
