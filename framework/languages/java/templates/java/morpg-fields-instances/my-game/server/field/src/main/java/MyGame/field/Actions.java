package MyGame.field;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class Actions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> reconnect(
            Field spot, Player actor, ZLinkMessageContext dispatch, JoinWorldRequest request) {
        actor.checkIdentity(request.playerId());
        spot.remember(actor);
        return actor.sendState();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> move(
            Field spot, Player actor, ZLinkMessageContext dispatch, MoveRequest request) {
        if (request.sequence() != actor.position().sequence() + 1)
            throw new IllegalArgumentException("Move sequence must increase by one");
        var destination = new EnterField(request.x(), request.sequence());
        if (!Rules.fieldId(request.x()).equals(spot.context().spotId())) {
            actor.move(destination);
            return CompletableFuture.completedFuture(null);
        }
        actor.apply(destination);
        spot.remember(actor);
        return actor.sendState();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliver(
            Field spot, Player actor, ZLinkMessageContext dispatch, BorderVisible message) {
        return actor.deliver(
                message.observerFieldId(),
                message,
                spot.state.visiblePlayers.containsKey(actor.context().actorId()));
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> createParty(
            Field spot, Player actor, ZLinkMessageContext dispatch, CreatePartyRequest request) {
        return spot.party(actor, request.partyId(), true);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> joinParty(
            Field spot, Player actor, ZLinkMessageContext dispatch, JoinPartyRequest request) {
        return spot.party(actor, request.partyId(), false);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> enterDungeon(
            Field spot, Player actor, ZLinkMessageContext dispatch, EnterDungeonRequest request) {
        return spot.enter(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> travel(
            Field spot, Player actor, ZLinkMessageContext dispatch, JoinDungeon request) {
        actor.context().joinSpot(request.dungeonId(), request).defer();
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliverField(
            Field spot, Player actor, ZLinkMessageContext dispatch, FieldView message) {
        return actor.deliver(
                message.fieldId(),
                message,
                spot.state.visiblePlayers.containsKey(actor.context().actorId()));
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> pendingDungeon(
            Field spot, Player actor, ZLinkMessageContext context, DungeonView message) {
        return actor.deliver(
                message.dungeonId(),
                message,
                spot.state.visiblePlayers.containsKey(actor.context().actorId()));
    }
}
