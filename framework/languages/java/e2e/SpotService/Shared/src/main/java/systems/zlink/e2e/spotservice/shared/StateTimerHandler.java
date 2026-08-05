package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class StateTimerHandler implements ZLinkSpotTimerHandler<UserSpot> {
    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        UserSpot spot,
        ZLinkTimerTick tick) {
        spot.timerTick(tick.deliveryIndex());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
