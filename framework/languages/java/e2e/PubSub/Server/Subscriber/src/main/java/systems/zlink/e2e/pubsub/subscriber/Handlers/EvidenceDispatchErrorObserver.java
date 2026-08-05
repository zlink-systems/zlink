package systems.zlink.e2e.pubsub.subscriber.Handlers;

import java.util.concurrent.CompletableFuture;
import systems.zlink.e2e.pubsub.subscriber.Infrastructure.EvidenceStore;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;

public final class EvidenceDispatchErrorObserver {
    private final EvidenceStore evidence;

    public EvidenceDispatchErrorObserver(EvidenceStore evidence) {
        this.evidence = evidence;
    }

    public CompletableFuture<Void> observe(systems.zlink.framework.configuration.ZLinkMessageFlowEvent error) {
        if (error.outcome() != ZLinkMessageFlowOutcome.ERROR) {
            return CompletableFuture.completedFuture(null);
        }
        evidence.record(
            "DispatchError",
            error.topic(),
            "observer",
            -1,
            error.errorReason() + "/" + error.errorAction() + "/" + error.packetName());
        return CompletableFuture.completedFuture(null);
    }
}
