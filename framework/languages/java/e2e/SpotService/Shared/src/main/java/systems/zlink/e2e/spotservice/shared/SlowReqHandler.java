package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class SlowReqHandler {
    @ZLinkSpotRequest
    public CompletionStage<Contracts.StateRes> handle(
        UserSpot spot,
        Contracts.SlowReq request) {
        try {
            Thread.sleep(1000);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("interrupted", error);
        }
        return CompletableFuture.completedFuture(new Contracts.StateRes(
            spot.context().spotId(),
            spot.context().nodeRid().toString(),
            spot.apply("slow:" + request.value())));
    }
}
