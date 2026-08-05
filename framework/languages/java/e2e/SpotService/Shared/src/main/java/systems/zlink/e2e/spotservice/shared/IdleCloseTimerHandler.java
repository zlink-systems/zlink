package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class IdleCloseTimerHandler implements ZLinkSpotTimerHandler<TimerScenarioSpot> {
    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        TimerScenarioSpot spot,
        ZLinkTimerTick tick) {
        spot.idleTick();
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
