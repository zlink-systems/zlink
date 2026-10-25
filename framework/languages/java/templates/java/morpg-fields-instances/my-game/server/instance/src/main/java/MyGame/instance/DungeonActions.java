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
        return actor.context().boundSession().send(message).submit();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliverParty(
            Dungeon spot, Player actor, ZLinkMessageContext dispatch, PartyView message) {
        return actor.context().boundSession().send(message).submit();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> enter(
            Dungeon spot, Player actor, ZLinkMessageContext dispatch, EnterDungeonRequest request) {
        return spot.enter(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> createParty(
            Dungeon spot, Player actor, ZLinkMessageContext dispatch, CreatePartyRequest request) {
        return spot.rejectTownRequest(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> joinParty(
            Dungeon spot, Player actor, ZLinkMessageContext dispatch, JoinPartyRequest request) {
        return spot.rejectTownRequest(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> travel(
            Dungeon spot, Player actor, ZLinkMessageContext dispatch, JoinDungeon request) {
        if (!request.dungeonId().equals(spot.context().spotId()))
            actor.context().joinSpot(request.dungeonId(), request).defer();
        return CompletableFuture.completedFuture(null);
    }
}
