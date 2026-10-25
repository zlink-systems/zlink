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
        return spot.move(actor, request);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliver(
            Field spot, Player actor, ZLinkMessageContext dispatch, BorderVisible message) {
        return actor.context().boundSession().send(message).submit();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> createParty(
            Field spot, Player actor, ZLinkMessageContext dispatch, CreatePartyRequest request) {
        return spot.party(actor, null, true);
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
    public CompletionStage<Void> deliverParty(
            Field spot, Player actor, ZLinkMessageContext dispatch, PartyView message) {
        return actor.context().boundSession().send(message).submit();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> query(
            Field spot, Player actor, ZLinkMessageContext dispatch, ViewRequest request) {
        return spot.show(actor);
    }
}
