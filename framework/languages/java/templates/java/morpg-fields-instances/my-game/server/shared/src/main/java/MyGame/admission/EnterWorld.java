package MyGame.admission;

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

    @ZLinkSpotActorSend
    public CompletionStage<Void> notificationPartyView(Entry spot, Player actor, ZLinkMessageContext context, PartyView message) {
        return actor.deliver(message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> notificationBorderVisible(Entry spot, Player actor, ZLinkMessageContext context, BorderVisible message) {
        return actor.deliver(message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> notificationDungeonView(Entry spot, Player actor, ZLinkMessageContext context, DungeonView message) {
        return actor.deliver(message);
    }
}
