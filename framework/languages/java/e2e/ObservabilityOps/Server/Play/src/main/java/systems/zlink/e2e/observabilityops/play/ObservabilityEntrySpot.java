package systems.zlink.e2e.observabilityops.play;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.automaticturn.shared.AwaitActor;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

public final class ObservabilityEntrySpot implements ZLinkEntrySpot<AwaitActor> {
    private final ZLinkEntrySpotContext context;

    public ObservabilityEntrySpot(ZLinkEntrySpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addHandler(ObservabilityActorJoinHandler.class);
        context.handlers().addHandler(ObservabilityActorPushHandler.class);
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
