package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class MultiNodeStateReqHandler {
    @ZLinkSpotRequest
    public CompletionStage<Contracts.MultiNodeStateRes> handle(
        MultiNodeSpot spot,
        Contracts.MultiNodeStateReq request) {
        int value = spot.add(request.delta());
        return CompletableFuture.completedFuture(new Contracts.MultiNodeStateRes(
            spot.context().spotId(),
            spot.context().nodeRid().toString(),
            value));
    }
}
