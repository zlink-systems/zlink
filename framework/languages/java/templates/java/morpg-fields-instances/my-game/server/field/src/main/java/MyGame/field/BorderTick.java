package MyGame.field;

import systems.zlink.framework.spots.*;

import java.util.concurrent.CompletionStage;

public final class BorderTick implements ZLinkSpotTimerHandler<Field> {
    public CompletionStage<Void> handle(Field spot, ZLinkTimerTick tick) {
        return spot.publishBorder();
    }
}
