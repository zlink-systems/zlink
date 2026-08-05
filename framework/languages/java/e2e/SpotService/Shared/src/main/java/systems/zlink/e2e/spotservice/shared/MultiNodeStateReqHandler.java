package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class MultiNodeStateReqHandler {
    @ZLinkSpotRequest
    public java.util.concurrent.CompletionStage<Contracts.MultiNodeStateRes> handle(
        MultiNodeSpot spot,
        Contracts.MultiNodeStateReq request) {
        int value = spot.add(request.delta());
        return java.util.concurrent.CompletableFuture.completedFuture(new Contracts.MultiNodeStateRes(
            spot.context().spotId(),
            spot.context().nodeRid().toString(),
            value));
    }
}
