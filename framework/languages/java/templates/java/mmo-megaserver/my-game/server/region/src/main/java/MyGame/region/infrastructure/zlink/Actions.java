package MyGame.region.infrastructure.zlink;

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
            Region spot, Player actor, ZLinkMessageContext dispatch, EnterRegionRequest request) {
        actor.checkIdentity(request.playerId());
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> query(
            Region spot, Player actor, ZLinkMessageContext dispatch, ViewRequest request) {
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> move(
            Region spot, Player actor, ZLinkMessageContext dispatch, MoveRequest request) {
        return spot.move(actor, request);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliver(
            Region spot, Player actor, ZLinkMessageContext dispatch, RegionView view) {
        return actor.context().boundSession().send(view).submit();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> dungeon(
            Region spot, Player actor, ZLinkMessageContext dispatch, OpenDungeonRequest request) {
        if (request.dungeonId() == null || request.dungeonId().isBlank())
            throw new IllegalArgumentException("Dungeon ID is required");
        return spots.getOrCreate(request.dungeonId(), Names.DUNGEON)
                .inMesh(Names.MESH)
                .submit()
                .thenCompose(
                        created -> {
                            if (created.state() == ZLinkSpotCreateState.REJECTED)
                                throw new IllegalStateException("Dungeon creation rejected");
                            return spot.context()
                                    .outbound()
                                    .requestToSpot(request.dungeonId(), new DungeonInfoRequest())
                                    .submit(DungeonOpened.class);
                        })
                .thenCompose(opened -> actor.context().boundSession().send(opened).submit());
    }
}
