package MyGame.match;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class Actions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> queue(
            Matchmaking spot, Player actor, ZLinkMessageContext context, QueueRequest request) {
        return spot.queue(actor, request);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> assign(
            Matchmaking spot, Player actor, ZLinkMessageContext context, RoomAssignment message) {
        actor.context()
                .joinSpot(
                        message.roomId(),
                        new RoomAdmission(actor.context().actorId(), message.roomId()))
                .defer();
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> reconnect(
            Room spot, Player actor, ZLinkMessageContext context, QueueRequest request) {
        if (!request.playerId().equals(actor.context().actorId()))
            throw new IllegalArgumentException("Reconnect identity mismatch");
        return spot.sendView(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> view(
            Room spot, Player actor, ZLinkMessageContext context, RoomViewRequest request) {
        return spot.sendView(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> move(
            Room spot, Player actor, ZLinkMessageContext context, MoveRequest request) {
        return spot.move(actor, request);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> shoot(
            Room spot, Player actor, ZLinkMessageContext context, ShootRequest request) {
        return spot.shoot(actor, request);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> forward(
            Room spot, Player actor, ZLinkMessageContext context, RoomView message) {
        return Room.reply(actor, message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> forward(
            Room spot, Player actor, ZLinkMessageContext context, MatchFound message) {
        return Room.reply(actor, message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> forward(
            Room spot, Player actor, ZLinkMessageContext context, MatchEnded message) {
        return Room.reply(actor, message);
    }
}
