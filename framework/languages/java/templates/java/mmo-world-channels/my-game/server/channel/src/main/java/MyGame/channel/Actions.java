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
        if (request.channel() > GameRules.MAX_INTEGER)
            return actor.context()
                    .boundSession()
                    .send(new ActionRejected(Rejection.INTEGER_OUT_OF_RANGE, 0))
                    .submit();
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
        return actor.earn()
                .thenCompose(
                        rejection ->
                                rejection == null
                                        ? spot.show(actor)
                                        : actor.context().boundSession().send(rejection).submit());
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> view(
            Zone spot, Player actor, ZLinkMessageContext context, ViewRequest request) {
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> channelView(
            Zone spot, Player actor, ZLinkMessageContext context, ChannelViewRequest request) {
        return spot.showChannelView(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> forward(
            Zone spot, Player actor, ZLinkMessageContext context, PlayerView message) {
        return actor.context().boundSession().send(message).submit();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> forwardChannelView(
            Zone spot, Player actor, ZLinkMessageContext context, ChannelView message) {
        return actor.context().boundSession().send(message).submit();
    }
}
