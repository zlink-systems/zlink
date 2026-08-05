package systems.zlink.e2e.channelegress.role;

import java.time.Duration;
import java.util.ArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.channelegress.shared.Contracts;
import systems.zlink.e2e.channelegress.shared.EvidenceState;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkTimer;

public final class Config12Spot implements ZLinkSpot<ZLinkActor> {
    private final ZLinkSpotContext context;
    private final EvidenceState evidence;
    private ZLinkTimer timer;

    public Config12Spot(ZLinkSpotContext context, EvidenceState evidence) {
        this.context = context;
        this.evidence = evidence;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addHandler(SpotWorkflowHandler.class);
    }

    @Override
    public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage ignored) {
        evidence.add("spot-initialize", "spot=" + context.spotId());
        return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
    }

    @Override
    public CompletionStage<Void> onJoinedActor(ZLinkActor ignored) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(ZLinkActor ignored) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onClosing() {
        evidence.add("spot-closing", "spot=" + context.spotId());
        return CompletableFuture.completedFuture(null);
    }

    CompletionStage<Void> startTimer(String name) {
        return context.addTimer(
                name,
                Duration.ofMillis(1),
                SpotWorkflowTimerHandler.class,
                null)
            .thenAccept(value -> timer = value);
    }

    void closeTimer() {
        ZLinkTimer current = timer;
        if (current != null) {
            current.close();
            timer = null;
        }
    }
}
