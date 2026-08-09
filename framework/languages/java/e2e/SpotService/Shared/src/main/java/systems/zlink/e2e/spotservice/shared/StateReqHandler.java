package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class StateReqHandler {
    @ZLinkSpotRequest
    public CompletionStage<Contracts.StateRes> handle(
        UserSpot spot,
        Contracts.StateReq request) {
        String value = request.op().equals("worker-start")
            ? spot.startWorker(request.op())
            : spot.apply(request.op());
        return CompletableFuture.completedFuture(new Contracts.StateRes(
            spot.context().spotId(),
            spot.context().nodeRid().toString(),
            value));
    }
}
