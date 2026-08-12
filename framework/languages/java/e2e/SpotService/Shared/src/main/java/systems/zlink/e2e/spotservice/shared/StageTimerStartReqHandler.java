package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class StageTimerStartReqHandler {
    @ZLinkSpotRequest
    public CompletionStage<Contracts.StageTimerStartRes> handle(
        UserSpot spot,
        Contracts.StageTimerStartReq request) {
        return CompletableFuture.completedFuture(spot.stage().startTimer(request));
    }
}
