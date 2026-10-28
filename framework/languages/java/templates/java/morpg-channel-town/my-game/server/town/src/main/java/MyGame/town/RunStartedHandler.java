package MyGame.town;

import MyGame.*;

import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup(Names.TOWN)
public final class RunStartedHandler implements ZLinkSpotPacketHandler<Town, RunStarted> {
    @Override
    public CompletionStage<Void> handle(Town spot, RunStarted message) {
        return spot.runStarted(message);
    }
}
