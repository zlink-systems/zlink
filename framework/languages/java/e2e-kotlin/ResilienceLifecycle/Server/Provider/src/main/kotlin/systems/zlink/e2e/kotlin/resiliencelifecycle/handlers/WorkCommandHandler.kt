package systems.zlink.e2e.kotlin.resiliencelifecycle.handlers

import systems.zlink.e2e.kotlin.resiliencelifecycle.Contracts
import systems.zlink.e2e.kotlin.resiliencelifecycle.ScenarioState
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
class WorkCommandHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingSendHandler<Contracts.WorkMsg> {
    override suspend fun handle(
        message: Contracts.WorkMsg,
        context: ZLinkMessageContext,
    ) {
        state.record("WorkMsg", message.value())
    }
}
