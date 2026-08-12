package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class StageProbeReqHandler {
    @ZLinkSpotRequest
    public CompletionStage<Contracts.StateRes> handle(
        UserSpot spot,
        Contracts.StageProbeReq request) {
        return CompletableFuture.completedFuture(spot.stage().apply(request));
    }
}
