package MyGame.match.infrastructure.zlink;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class Join {
    // The HTTP lobby creates the Room; the Entry Spot only defers the join to it.
    @ZLinkSpotActorSend
    public CompletionStage<Void> join(
            Entry entry, Player actor, ZLinkMessageContext dispatch, JoinRoomRequest request) {
        if (!actor.context().actorId().equals(request.playerId()))
            throw new IllegalArgumentException("Player identity differs from Actor");
        actor.context().joinSpot(request.roomId(), request).defer();
        return CompletableFuture.completedFuture(null);
    }
}
