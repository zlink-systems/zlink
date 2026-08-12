package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class IdleCloseTimerHandler implements ZLinkSpotTimerHandler<TimerScenarioSpot> {
    @Override
    public CompletionStage<Void> handle(
        TimerScenarioSpot spot,
        ZLinkTimerTick tick) {
        spot.idleTick();
        return CompletableFuture.completedFuture(null);
    }
}
