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
        String reason = ChannelRules.validate(request.channel());
        if (reason != null)
            return actor.context().boundSession().send(new ActionRejected(reason)).submit();
        actor.context().joinSpot(Names.townId(request.channel()), request).defer();
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> notificationTownView(Entry spot, Player actor, ZLinkMessageContext context, TownView message) {
        return actor.deliver(message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> notificationDungeonView(Entry spot, Player actor, ZLinkMessageContext context, DungeonView message) {
        return actor.deliver(message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> notificationActionRejected(Entry spot, Player actor, ZLinkMessageContext context, ActionRejected message) {
        return actor.deliver(message);
    }
}
