package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class StageTimerHandler implements ZLinkSpotTimerHandler<UserSpot> {
    @Override
    public CompletionStage<Void> handle(
        UserSpot spot,
        ZLinkTimerTick tick) {
        spot.record("StageTimer", tick.name());
        return CompletableFuture.completedFuture(null);
    }
}
