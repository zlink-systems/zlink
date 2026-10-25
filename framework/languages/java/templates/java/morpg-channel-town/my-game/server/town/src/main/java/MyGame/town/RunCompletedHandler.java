package MyGame.town;

import MyGame.*;

import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup(Names.TOWN)
public final class RunCompletedHandler implements ZLinkSpotPacketHandler<Town, RunCompleted> {
    @Override
    public CompletionStage<Void> handle(Town spot, RunCompleted message) {
        return spot.runCompleted(message);
    }
}
