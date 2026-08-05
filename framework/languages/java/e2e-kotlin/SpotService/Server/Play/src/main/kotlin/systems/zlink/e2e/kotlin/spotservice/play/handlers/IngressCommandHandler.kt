package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup

@ZLinkHandlerGroup(Contracts.CHANNEL_HANDLER_GROUP)
class IngressCommandHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingSendHandler<Contracts.OutboundMsg> {
    override suspend fun handle(message: Contracts.OutboundMsg, context: ZLinkMessageContext) {
        state.record("IngressCommand", "channel", message.value)
    }
}
