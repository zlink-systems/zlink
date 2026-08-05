package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class StateReqHandler {
    @ZLinkSpotRequest
    public java.util.concurrent.CompletionStage<Contracts.StateRes> handle(
        UserSpot spot,
        Contracts.StateReq request) {
        String value = request.op().equals("worker-start")
            ? spot.startWorker(request.op())
            : spot.apply(request.op());
        return java.util.concurrent.CompletableFuture.completedFuture(new Contracts.StateRes(
            spot.context().spotId(),
            spot.context().nodeRid().toString(),
            value));
    }
}
