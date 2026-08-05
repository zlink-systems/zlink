package systems.zlink.e2e.kotlin.resiliencelifecycle.handlers

import systems.zlink.e2e.kotlin.resiliencelifecycle.Contracts
import systems.zlink.e2e.kotlin.resiliencelifecycle.ScenarioState
import systems.zlink.framework.ZLinkMessageContext
import kotlinx.coroutines.delay
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
class WorkRequestHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingRequestHandler<Contracts.WorkReq, Contracts.WorkRes> {
    override suspend fun handle(
        request: Contracts.WorkReq,
        context: ZLinkMessageContext,
    ): Contracts.WorkRes {
        when {
            state.grayFailure() && request.value().startsWith("b6-gray-") -> {
                state.record("GrayFailureInjected", request.value())
                throw IllegalStateException("gray failure")
            }
            request.value() == "slow" || request.value().startsWith("b2-slow-") -> {
                state.record("SlowStarted", request.value())
                state.awaitSlowRelease()
                state.record("SlowCompleted", request.value())
            }
            request.value() == "timeout" -> {
                state.record("TimeoutStarted", request.value())
                delay(1500)
                state.record("TimeoutCompleted", request.value())
            }
            else -> state.record("WorkReq", request.value())
        }
        return Contracts.WorkRes("work:${request.value()}", state.providerRid())
    }

}
