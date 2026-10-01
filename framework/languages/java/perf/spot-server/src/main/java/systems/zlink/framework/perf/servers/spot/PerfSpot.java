package systems.zlink.framework.perf.servers.spot;

import systems.zlink.framework.perf.PerfActor;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// The Actor-less User Spot every Spot role hosts (§10): SpotWide, with no Actor member, so the membership callbacks are
// never called. A concrete Spot registers its typed handlers in its constructor and holds no other logic.
public abstract class PerfSpot implements ZLinkSpot<PerfActor> {
    private final ZLinkSpotContext context;

    protected PerfSpot(ZLinkSpotContext context) {
        this.context = context;
    }

    @Override
    public final ZLinkSpotContext context() {
        return context;
    }

    @Override
    public final CompletionStage<Void> onJoinedActor(PerfActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public final CompletionStage<Void> onLeaveActor(PerfActor actor) {
        return CompletableFuture.completedFuture(null);
    }
}
