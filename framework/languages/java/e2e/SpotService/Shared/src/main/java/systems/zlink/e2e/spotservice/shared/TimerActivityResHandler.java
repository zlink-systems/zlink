package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class TimerActivityResHandler {
    @ZLinkSpotRequest
    public java.util.concurrent.CompletionStage<Contracts.TimerActivityRes> handle(
        TimerScenarioSpot spot,
        String request) {
        return java.util.concurrent.CompletableFuture.completedFuture(spot.status());
    }
}
