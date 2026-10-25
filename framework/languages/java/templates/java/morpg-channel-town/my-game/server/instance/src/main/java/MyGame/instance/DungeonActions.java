package MyGame.instance;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class DungeonActions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> login(
            Dungeon spot, Player actor, ZLinkMessageContext context, JoinTownRequest request) {
        actor.checkIdentity(request.playerId());
        return spot.showWithParty(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> view(
            Dungeon spot, Player actor, ZLinkMessageContext context, ViewRequest request) {
        return spot.showWithParty(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> advance(
            Dungeon spot, Player actor, ZLinkMessageContext context, AdvanceRequest request) {
        return spot.advance(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliver(
            Dungeon spot, Player actor, ZLinkMessageContext context, DungeonView request) {
        return actor.context().boundSession().send(request).submit();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> enterAgain(
            Dungeon spot, Player actor, ZLinkMessageContext context, EnterDungeonRequest request) {
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> travelAgain(
            Dungeon spot, Player actor, ZLinkMessageContext context, JoinDungeon request) {
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> changeChannel(
            Dungeon spot, Player actor, ZLinkMessageContext context, ChangeChannelRequest request) {
        return actor.context()
                .boundSession()
                .send(new ActionRejected(Rules.RUN_IN_PROGRESS))
                .submit();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliverTownView(
            Dungeon spot, Player actor, ZLinkMessageContext context, TownView request) {
        return actor.context().boundSession().send(request).submit();
    }
}
