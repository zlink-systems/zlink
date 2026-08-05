package systems.zlink.e2e.spotservice.shared;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

public final class MismatchedSpot implements ZLinkSpot<ZLinkActor> {
    private final ZLinkSpotContext context;

    @Override
    public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    public MismatchedSpot(ZLinkSpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public void configure() {
    }
}
