package MyGame.channel;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class Actions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> enter(
            Zone spot, Player actor, ZLinkMessageContext context, EnterWorldRequest request) {
        actor.checkIdentity(request.playerId());
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> change(
            Zone spot, Player actor, ZLinkMessageContext context, ChangeChannelRequest request) {
        actor.context()
                .joinSpot(
                        Names.zone(request.channel()),
                        new EnterWorldRequest(actor.context().actorId()))
                .defer();
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> earn(
            Zone spot, Player actor, ZLinkMessageContext context, EarnRequest request) {
        return actor.earn(request.amount()).thenCompose(v -> spot.show(actor));
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> view(
            Zone spot, Player actor, ZLinkMessageContext context, ViewRequest request) {
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> forward(
            Zone spot, Player actor, ZLinkMessageContext context, PlayerView message) {
        return actor.context().boundSession().send(message).submit();
    }
}
