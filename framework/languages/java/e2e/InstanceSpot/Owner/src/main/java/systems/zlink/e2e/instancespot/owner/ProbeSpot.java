package systems.zlink.e2e.instancespot.owner;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpotClosingContext;

public final class ProbeSpot implements ZLinkInstanceSpot {
    private final ZLinkInstanceSpotContext context;
    private final EvidenceStore evidence;
    private final GateController gates;
    private final AtomicLong handlerSequence = new AtomicLong();
    private final AtomicInteger activeHandlers = new AtomicInteger();

    public ProbeSpot(
        ZLinkInstanceSpotContext context,
        EvidenceStore evidence,
        GateController gates) {
        this.context = context;
        this.evidence = evidence;
        this.gates = gates;
        evidence.record(
            "FACTORY",
            context.spotId(),
            "",
            "",
            context.objectGeneration(),
            0,
            "constructor");
    }

    @Override
    public ZLinkInstanceSpotContext context() {
        return context;
    }

    EvidenceStore evidence() {
        return evidence;
    }

    GateController gates() {
        return gates;
    }

    long nextHandlerSequence() {
        return handlerSequence.incrementAndGet();
    }

    int enterHandler(String operationId, String payload) {
        int active = activeHandlers.incrementAndGet();
        evidence.record(
            "HANDLER_ENTER",
            context.spotId(),
            operationId,
            payload,
            context.objectGeneration(),
            active,
            "request");
        return active;
    }

    void leaveHandler(String operationId, String payload) {
        int active = activeHandlers.decrementAndGet();
        evidence.record(
            "HANDLER_COMMIT",
            context.spotId(),
            operationId,
            payload,
            context.objectGeneration(),
            active,
            "domain-commit");
    }

    @Override
    public void configure() {
        context.handlers().addPacket(ProbeSendHandler.class);
        context.handlers().addPacket(CloseHandler.class);
    }

    @Override
    public CompletionStage<Void> onInitialize() {
        evidence.record(
            "INITIALIZE",
            context.spotId(),
            "",
            "",
            context.objectGeneration(),
            0,
            "ready");
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onClosing(ZLinkSpotClosingContext closing) {
        evidence.record(
            "CLOSING",
            context.spotId(),
            "",
            "",
            context.objectGeneration(),
            0,
            closing.reason().name());
        return CompletableFuture.completedFuture(null);
    }
}
