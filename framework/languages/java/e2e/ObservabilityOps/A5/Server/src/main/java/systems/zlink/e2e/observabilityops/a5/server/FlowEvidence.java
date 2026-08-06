package systems.zlink.e2e.observabilityops.a5.server;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowObserver;

public final class FlowEvidence implements ZLinkMessageFlowObserver {
    private final CopyOnWriteArrayList<ZLinkMessageFlowEvent> events =
        new CopyOnWriteArrayList<>();

    @Override
    public CompletionStage<Void> onMessageFlow(ZLinkMessageFlowEvent flow) {
        events.add(flow);
        return CompletableFuture.completedFuture(null);
    }

    public List<ZLinkMessageFlowEvent> snapshot() {
        return List.copyOf(events);
    }
}
