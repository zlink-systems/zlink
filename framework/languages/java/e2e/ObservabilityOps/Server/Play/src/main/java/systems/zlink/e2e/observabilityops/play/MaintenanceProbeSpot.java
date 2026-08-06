package systems.zlink.e2e.observabilityops.play;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletableFuture;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.e2e.automaticturn.shared.AwaitActor;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotClosingContext;
import systems.zlink.framework.spots.ZLinkSpotContext;

/** User Spot used only to observe the public closing deadline contract. */
public final class MaintenanceProbeSpot implements ZLinkSpot<AwaitActor> {
    private final ZLinkSpotContext context;
    private final MaintenanceGate gate;

    public MaintenanceProbeSpot(ZLinkSpotContext context, MaintenanceGate gate) {
        this.context = context;
        this.gate = gate;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onClosing(ZLinkSpotClosingContext closing) {
        return gate.awaitClosing(closing);
    }

    @Override
    public CompletionStage<Void> onJoinedActor(AwaitActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(AwaitActor actor) {
        return CompletableFuture.completedFuture(null);
    }
}
