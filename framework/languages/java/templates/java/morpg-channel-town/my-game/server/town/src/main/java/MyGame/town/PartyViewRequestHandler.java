package MyGame.town;

import MyGame.*;

import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup(Names.TOWN)
public final class PartyViewRequestHandler
        implements ZLinkSpotPacketHandler<Town, PartyViewRequest> {
    @Override
    public CompletionStage<Void> handle(Town spot, PartyViewRequest message) {
        return spot.showParty(message.playerId());
    }
}
