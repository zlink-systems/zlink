package MyGame.zone;

import systems.zlink.framework.spots.*;

import java.util.concurrent.CompletionStage;

public final class BorderTick implements ZLinkSpotTimerHandler<Zone> {
    public CompletionStage<Void> handle(Zone spot, ZLinkTimerTick tick) {
        return spot.publishBorder();
    }
}
