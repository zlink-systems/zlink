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
        return spot.reconnect(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> move(
            Zone spot, Player actor, ZLinkMessageContext dispatch, MoveRequest request) {
        return spot.move(actor, request);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliverAppeared(
            Zone spot, Player actor, ZLinkMessageContext dispatch, PlayerAppeared message) {
        return spot.deliverAppeared(actor, dispatch, message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliverMoved(
            Zone spot, Player actor, ZLinkMessageContext dispatch, PlayerMoved message) {
        return spot.deliverMoved(actor, dispatch, message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliverLeft(
            Zone spot, Player actor, ZLinkMessageContext dispatch, PlayerLeft message) {
        return spot.deliverLeft(actor, dispatch, message);
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
