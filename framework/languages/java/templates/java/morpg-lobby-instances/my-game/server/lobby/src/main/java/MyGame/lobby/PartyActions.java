package MyGame.lobby;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class PartyActions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> login(
            Party spot, Player actor, ZLinkMessageContext context, JoinLobbyRequest request) {
        actor.checkIdentity(request.playerId());
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> view(
            Party spot, Player actor, ZLinkMessageContext context, ViewRequest request) {
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> ready(
            Party spot, Player actor, ZLinkMessageContext context, ReadyRequest request) {
        return spot.ready(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> enter(
            Party spot, Player actor, ZLinkMessageContext context, EnterDungeonRequest request) {
        return spot.enter(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> travel(
            Party spot, Player actor, ZLinkMessageContext context, JoinDungeon request) {
        actor.context().joinSpot(request.dungeonId(), request).defer();
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliver(
            Party spot, Player actor, ZLinkMessageContext context, PartyView request) {
        return actor.context().boundSession().send(request).submit();
    }
    @ZLinkSpotActorSend
    public CompletionStage<Void> reject(Party spot, Player actor, ZLinkMessageContext context, ActionRejected r) { return actor.context().boundSession().send(r).submit(); }
}
