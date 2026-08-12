package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup

@ZLinkHandlerGroup(Contracts.CHANNEL_HANDLER_GROUP)
class NoopIngressHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingRequestHandler<Contracts.StateReq, Contracts.StateRes> {
    override suspend fun handle(
        request: Contracts.StateReq,
        context: ZLinkMessageContext,
    ): Contracts.StateRes {
        state.record("IngressRequest", "channel", request.op)
        return Contracts.StateRes("", state.nodeRid(), request.op)
    }
}
