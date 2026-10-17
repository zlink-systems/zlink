package MyGame.zone;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class Actions {
    private final ZLinkSpotManager spots;

    public Actions(ZLinkSpotManager spots) {
        this.spots = spots;
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> reconnect(
            Zone spot, Player actor, ZLinkMessageContext dispatch, JoinWorldRequest request) {
        actor.checkIdentity(request.playerId());
        spot.remember(actor);
        return actor.sendState();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> move(
            Zone spot, Player actor, ZLinkMessageContext dispatch, MoveRequest request) {
        if (request.sequence() != actor.position().sequence() + 1)
            throw new IllegalArgumentException("Move sequence must increase by one");
        var destination = new EnterZone(request.x(), request.sequence());
        if (!Rules.zoneId(request.x()).equals(spot.context().spotId())) {
            actor.move(destination);
            return CompletableFuture.completedFuture(null);
        }
        actor.apply(destination);
        spot.remember(actor);
        return actor.sendState();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliver(
            Zone spot, Player actor, ZLinkMessageContext dispatch, BorderVisible message) {
        return spot.deliver(actor, message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> dungeon(
            Zone spot, Player actor, ZLinkMessageContext dispatch, OpenDungeonRequest request) {
        return spots.getOrCreate(request.dungeonId(), Names.DUNGEON)
                .inMesh(Names.MESH)
                .submit()
                .thenCompose(
                        created -> {
                            if (created.state() == ZLinkSpotCreateState.REJECTED)
                                throw new IllegalStateException("Dungeon creation rejected");
                            return spot.context()
                                    .outbound()
                                    .requestToSpot(request.dungeonId(), request)
                                    .submit(DungeonOpened.class);
                        })
                .thenCompose(opened -> actor.context().boundSession().send(opened).submit());
    }
}
