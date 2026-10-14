package MyGame.instance;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class DungeonActions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> login(
            Dungeon spot, Player actor, ZLinkMessageContext context, JoinLobbyRequest request) {
        actor.checkIdentity(request.playerId());
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> view(
            Dungeon spot, Player actor, ZLinkMessageContext context, ViewRequest request) {
        return spot.show(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> advance(
            Dungeon spot, Player actor, ZLinkMessageContext context, AdvanceRequest request) {
        return spot.advance();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> deliver(
            Dungeon spot, Player actor, ZLinkMessageContext context, DungeonView request) {
        return actor.context().boundSession().send(request).submit();
    }
}
