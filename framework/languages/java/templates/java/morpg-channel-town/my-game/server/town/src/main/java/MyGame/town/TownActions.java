package MyGame.town;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class TownActions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> login(
            Town spot, Player actor, ZLinkMessageContext context, JoinTownRequest request) {
        actor.checkIdentity(request.playerId());
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> change(
            Town spot, Player actor, ZLinkMessageContext context, ChangeChannelRequest request) {
        return spot.changeChannel(actor, request.channel());
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> create(
            Town spot, Player actor, ZLinkMessageContext context, CreatePartyRequest request) {
        return spot.party(actor, null, true);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> join(
            Town spot, Player actor, ZLinkMessageContext context, JoinPartyRequest request) {
        return spot.party(actor, request.partyId(), false);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> enter(
            Town spot, Player actor, ZLinkMessageContext context, EnterDungeonRequest request) {
        return spot.enter(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> travel(
            Town spot, Player actor, ZLinkMessageContext context, JoinDungeon request) {
        actor.context().joinSpot(request.dungeonId(), request).defer();
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> view(
            Town spot, Player actor, ZLinkMessageContext context, ViewRequest request) {
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliver(
            Town spot, Player actor, ZLinkMessageContext context, TownView request) {
        return actor.context().boundSession().send(request).submit();
    }
}
