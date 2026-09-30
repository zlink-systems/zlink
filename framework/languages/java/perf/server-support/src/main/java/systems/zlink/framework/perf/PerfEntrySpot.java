package systems.zlink.framework.perf;

import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// The request cells answer with the typed reply; the send-send cell answers with a public Channel send (§10.10).
public final class PerfEntrySpot implements ZLinkEntrySpot<PerfActor> {
    private final ZLinkEntrySpotContext context;

    public PerfEntrySpot(ZLinkEntrySpotContext context, RoleConfig config) {
        this.context = context;
        if ("send-send".equals(config.mode())) {
            context.handlers().addHandler(ActorEchoSendHandler.class);
        } else {
            context.handlers().addHandler(ActorEchoRequestHandler.class);
        }
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onJoinedActor(PerfActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(PerfActor actor) {
        return CompletableFuture.completedFuture(null);
    }
}
