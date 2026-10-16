package MyGame;

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
        return spot.join(actor, request.partyId(), true);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> join(
            Entry spot, Player actor, ZLinkMessageContext context, JoinPartyRequest request) {
        return spot.join(actor, request.partyId(), false);
    }
}
