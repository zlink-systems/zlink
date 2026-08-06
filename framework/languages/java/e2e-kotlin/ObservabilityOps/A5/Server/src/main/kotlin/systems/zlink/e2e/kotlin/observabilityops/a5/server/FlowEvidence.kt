package systems.zlink.e2e.kotlin.observabilityops.a5.server

import systems.zlink.framework.configuration.ZLinkMessageFlowEvent
import systems.zlink.framework.configuration.ZLinkMessageFlowObserver
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CopyOnWriteArrayList

class FlowEvidence : ZLinkMessageFlowObserver {
    private val events = CopyOnWriteArrayList<ZLinkMessageFlowEvent>()

    override fun onMessageFlow(flow: ZLinkMessageFlowEvent) =
        CompletableFuture.runAsync { events.add(flow) }

    fun snapshot(): List<ZLinkMessageFlowEvent> = events.toList()
}
