package MyGame.admission;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class EntryActions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> login(
            Entry spot, Player actor, ZLinkMessageContext context, JoinLobbyRequest request) {
        actor.checkIdentity(request.playerId());
        return actor.context().boundSession().send(new LobbyView()).submit();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> create(
            Entry spot, Player actor, ZLinkMessageContext context, CreatePartyRequest request) {
        return spot.join(actor, Names.newPartyId(), true);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> join(
            Entry spot, Player actor, ZLinkMessageContext context, JoinPartyRequest request) {
        return spot.join(actor, request.partyId(), false);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> notificationPartyView(Entry spot, Player actor, ZLinkMessageContext context, PartyView message) {
        return actor.deliver(message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> notificationDungeonView(Entry spot, Player actor, ZLinkMessageContext context, DungeonView message) {
        return actor.deliver(message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> notificationActionRejected(Entry spot, Player actor, ZLinkMessageContext context, ActionRejected message) {
        return actor.deliver(message);
    }
}
