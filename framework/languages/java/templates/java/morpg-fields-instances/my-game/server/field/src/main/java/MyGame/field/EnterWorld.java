package MyGame.field;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class EnterWorld {
    @ZLinkSpotActorSend
    public CompletionStage<Void> join(
            Entry entry, Player actor, ZLinkMessageContext dispatch, JoinWorldRequest request) {
        actor.checkIdentity(request.playerId());
        actor.move(new EnterField(request.x(), 0));
        return CompletableFuture.completedFuture(null);
    }
}
