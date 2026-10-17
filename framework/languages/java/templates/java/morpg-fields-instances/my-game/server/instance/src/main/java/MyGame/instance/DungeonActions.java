package MyGame.instance;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class DungeonActions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> reconnect(
            Dungeon spot, Player actor, ZLinkMessageContext dispatch, JoinWorldRequest request) {
        actor.checkIdentity(request.playerId());
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> query(
            Dungeon spot, Player actor, ZLinkMessageContext dispatch, ViewRequest request) {
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> advance(
            Dungeon spot, Player actor, ZLinkMessageContext dispatch, AdvanceRequest request) {
        return spot.advance(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliver(
            Dungeon spot, Player actor, ZLinkMessageContext dispatch, DungeonView message) {
        return actor.deliver(message.dungeonId(), message, true);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> departedBorderVisible(
            Dungeon spot, Player actor, ZLinkMessageContext context, BorderVisible message) {
        return actor.deliver(message.observerFieldId(), message, true);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> departedFieldView(
            Dungeon spot, Player actor, ZLinkMessageContext context, FieldView message) {
        return actor.deliver(message.fieldId(), message, true);
    }
}
