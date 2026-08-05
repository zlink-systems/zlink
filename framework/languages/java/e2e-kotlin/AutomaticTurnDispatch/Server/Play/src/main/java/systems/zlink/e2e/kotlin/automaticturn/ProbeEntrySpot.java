package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;

public final class ProbeEntrySpot implements ZLinkEntrySpot<ProbeActor> {
    private final ZLinkEntrySpotContext context;

    public ProbeEntrySpot(ZLinkEntrySpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addHandler(EntryActorJoinHandler.class);
        context.handlers().addHandler(EntryActorProbeHandler.class);
        context.handlers().addHandler(EntryActorAwaitHandler.class);
        context.handlers().addHandler(EntryActorFastHandler.class);
        context.handlers().addHandler(EntryActorJoinAwaitHandler.class);
        context.handlers().addHandler(EntryActorPushAwaitHandler.class);
    }

    @Override
    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId,
        ZLinkMessage request) {
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
    }

    @Override
    public CompletionStage<Void> onJoinedActor(ProbeActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(ProbeActor actor) {
        return CompletableFuture.completedFuture(null);
    }
}
