package MyGame.region.infrastructure.zlink;

import MyGame.*;
import MyGame.region.domain.Positions;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class Region implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient actors;
    final Positions positions = new Positions();

    public Region(ZLinkSpotContext context, NodeConfig config, ZLinkActorClient actors) {
        this.context = context;
        this.config = config;
        this.actors = actors;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
        var join = request.decode(JoinRegion.class);
        if (!context.spotId().equals(Names.regionId(join.region(), join.index())))
            throw new IllegalArgumentException("Region join identity mismatch");
        if (!positions.admit(actorId, config.capacity()))
            return CompletableFuture.completedFuture(
                    ZLinkSpotActorJoinResult.reject(
                            ZLinkMessage.of(
                                    new RegionFull(
                                            Names.REGION_FULL, join.region(), join.index()))));
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return broadcast();
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        positions.leave(actor.context().actorId());
        return broadcast();
    }

    // A disconnected Session retains membership and position until explicit leave.
    public CompletionStage<Void> onDisconnectActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    private RegionView view() {
        return new RegionView(context.spotId(), config.id(), positions.view());
    }

    public CompletionStage<Void> show(Player actor) {
        return actor.context().boundSession().send(view()).submit();
    }

    public CompletionStage<Void> move(Player actor, MoveRequest request) {
        var id = actor.context().actorId();
        positions.move(id, request.x(), request.sequence());
        // The request handler replies directly; other recipients use the public Actor route.
        var view = view();
        return actor.context()
                .boundSession()
                .send(view)
                .submit()
                .thenCompose(ignored -> broadcastExcept(view, id));
    }

    private CompletionStage<Void> broadcast() {
        return broadcastExcept(view(), null);
    }

    private CompletionStage<Void> broadcastExcept(RegionView view, String excluded) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var id : positions.playerIds())
            if (!id.equals(excluded))
                sends = sends.thenCompose(ignored -> actors.sendToActor(id, view).submit());
        return sends;
    }
}
