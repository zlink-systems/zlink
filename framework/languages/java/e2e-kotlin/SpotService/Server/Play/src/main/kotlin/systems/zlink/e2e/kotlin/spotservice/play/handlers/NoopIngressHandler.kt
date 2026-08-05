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
) : ZLinkSuspendingRequestHandler<String, String> {
    override suspend fun handle(request: String, context: ZLinkMessageContext): String {
        state.record("IngressRequest", "channel", request)
        return request
    }
}
