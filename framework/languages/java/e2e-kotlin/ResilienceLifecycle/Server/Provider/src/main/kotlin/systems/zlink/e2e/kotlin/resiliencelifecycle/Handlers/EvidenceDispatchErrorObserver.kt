package systems.zlink.e2e.kotlin.resiliencelifecycle

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent
import systems.zlink.framework.configuration.ZLinkMessageFlowObserver
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome

class EvidenceDispatchErrorObserver(
    private val state: ScenarioState,
) : ZLinkMessageFlowObserver {
    override fun onMessageFlow(flow: ZLinkMessageFlowEvent): CompletionStage<Void> {
        if (flow.outcome() != ZLinkMessageFlowOutcome.ERROR) {
            return CompletableFuture.completedFuture(null)
        }
        state.record(
            "DispatchError",
            "${flow.errorReason()}/${flow.errorAction()}/${flow.packetName()}",
        )
        if (state.observerThrows()) {
            throw IllegalStateException("dispatch observer failure")
        }
        return CompletableFuture.completedFuture(null)
    }
}
