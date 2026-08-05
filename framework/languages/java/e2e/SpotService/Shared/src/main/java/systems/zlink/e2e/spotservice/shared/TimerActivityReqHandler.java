package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class TimerActivityReqHandler {
    @ZLinkSpotRequest
    public java.util.concurrent.CompletionStage<Contracts.TimerActivityRes> handle(
        TimerScenarioSpot spot,
        Contracts.TimerActivityReq request) {
        spot.activity(request.value());
        return java.util.concurrent.CompletableFuture.completedFuture(spot.status());
    }
}
