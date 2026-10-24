package MyGame.match.infrastructure.zlink;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup(Names.PLAYER)
public final class Actions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> join(
            Room room, Player actor, ZLinkMessageContext context, JoinRoomRequest request) {
        room.checkJoin(actor.context().actorId(), request);
        return room.current(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> view(
            Room room, Player actor, ZLinkMessageContext context, RoomViewRequest request) {
        return room.current(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> move(
            Room room, Player actor, ZLinkMessageContext context, MoveRequest request) {
        return room.move(actor, request);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> shoot(
            Room room, Player actor, ZLinkMessageContext context, ShootRequest request) {
        return room.shoot(actor, request);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> forward(
            Room room, Player actor, ZLinkMessageContext context, RoomView message) {
        return Room.reply(actor, message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> forward(
            Room room, Player actor, ZLinkMessageContext context, MatchEnded message) {
        return Room.reply(actor, message);
    }
}
