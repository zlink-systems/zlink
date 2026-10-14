package MyGame.match;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class EnterQueue {
    @ZLinkSpotActorSend
    public CompletionStage<Void> queue(
            Entry entry, Player actor, ZLinkMessageContext context, QueueRequest request) {
        if (!actor.context().actorId().equals(request.playerId()))
            throw new IllegalArgumentException("Queue identity mismatch");
        actor.context().joinSpot(Names.MATCHMAKING_ID, request).defer();
        return CompletableFuture.completedFuture(null);
    }
}
