package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class StageProbeReqHandler {
    @ZLinkSpotRequest
    public java.util.concurrent.CompletionStage<Contracts.StateRes> handle(
        UserSpot spot,
        Contracts.StageProbeReq request) {
        return java.util.concurrent.CompletableFuture.completedFuture(spot.stage().apply(request));
    }
}
