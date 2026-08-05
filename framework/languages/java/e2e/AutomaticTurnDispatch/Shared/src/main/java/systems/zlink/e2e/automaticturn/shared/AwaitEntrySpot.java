package systems.zlink.e2e.automaticturn.shared;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;

public final class AwaitEntrySpot implements ZLinkEntrySpot<AwaitActor> {
    private final ZLinkEntrySpotContext context;

    public AwaitEntrySpot(ZLinkEntrySpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addHandler(AwaitProbeHandlers.ActorJoinHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.ActorAwaitHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.ActorFastHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.ActorJoinAwaitHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.ActorPushNotifyAwaitHandler.class);
    }

    @Override
    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId,
        ZLinkMessage request) {
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
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
