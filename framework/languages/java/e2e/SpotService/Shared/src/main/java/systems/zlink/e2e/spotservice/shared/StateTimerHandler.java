package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class StateTimerHandler implements ZLinkSpotTimerHandler<UserSpot> {
    @Override
    public CompletionStage<Void> handle(
        UserSpot spot,
        ZLinkTimerTick tick) {
        spot.timerTick(tick.deliveryIndex());
        return CompletableFuture.completedFuture(null);
    }
}
