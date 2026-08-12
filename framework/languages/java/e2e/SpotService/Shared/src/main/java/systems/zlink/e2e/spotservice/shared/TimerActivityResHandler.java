package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class TimerActivityResHandler {
    @ZLinkSpotRequest
    public CompletionStage<Contracts.TimerActivityRes> handle(
        TimerScenarioSpot spot,
        String request) {
        return CompletableFuture.completedFuture(spot.status());
    }
}
