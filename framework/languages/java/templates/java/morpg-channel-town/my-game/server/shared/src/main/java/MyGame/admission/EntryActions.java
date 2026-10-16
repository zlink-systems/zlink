package MyGame.admission;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class EntryActions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> join(
            Entry spot, Player actor, ZLinkMessageContext context, JoinTownRequest request) {
        actor.checkIdentity(request.playerId());
        actor.context().joinSpot(Names.townId(request.channel()), request).defer();
        return CompletableFuture.completedFuture(null);
    }
}
