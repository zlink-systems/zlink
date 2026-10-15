package MyGame.channel;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class EntryActions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> enter(
            Entry spot, Player actor, ZLinkMessageContext context, EnterWorldRequest request) {
        actor.checkIdentity(request.playerId());
        actor.context().joinSpot(Names.zone(Names.FIRST_CHANNEL), request).defer();
        return CompletableFuture.completedFuture(null);
    }
}
