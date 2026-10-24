package MyGame.region.infrastructure.zlink;

import MyGame.*;

import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class Entry implements ZLinkEntrySpot<Player> {
    private final ZLinkEntrySpotContext context;
    private final ZLinkSpotManager spots;

    public Entry(ZLinkEntrySpotContext context, ZLinkSpotManager spots) {
        this.context = context;
        this.spots = spots;
    }

    public ZLinkEntrySpotContext context() {
        return context;
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> select(Player actor, String region, int index) {
        var id = Names.regionId(region, index);
        return spots.getOrCreate(id, Names.REGION)
                .inMesh(Names.MESH)
                .submit()
                .thenAccept(
                        created -> {
                            if (created.state() == ZLinkSpotCreateState.REJECTED)
                                throw new IllegalStateException("Region creation rejected: " + id);
                            actor.context().joinSpot(id, new JoinRegion(region, index)).defer();
                        });
    }
}
