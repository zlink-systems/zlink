package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class StageTimerStartReqHandler {
    @ZLinkSpotRequest
    public java.util.concurrent.CompletionStage<Contracts.StageTimerStartRes> handle(
        UserSpot spot,
        Contracts.StageTimerStartReq request) {
        return java.util.concurrent.CompletableFuture.completedFuture(spot.stage().startTimer(request));
    }
}
