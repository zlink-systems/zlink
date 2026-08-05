package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.spots.ZLinkSpotActorRequestContext;

public final class EntryActorProbeHandler {
    @ZLinkSpotActorRequest(packetName = "ProbeReq")
    public CompletionStage<Contracts.ProbeRes> handle(
        ProbeEntrySpot spot,
        ProbeActor actor,
        ZLinkSpotActorRequestContext context,
        Contracts.ProbeReq request) {
        if (request.millis() <= 0) {
            return CompletableFuture.completedFuture(
                reply(spot, request, "immediate:" + request.op()));
        }
        return spot.context().outbound()
            .requestToChannel(Contracts.DELAY_CHANNEL, new Contracts.DelayReq(request.op(), request.millis()))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.DelayRes.class)
            .thenApply(delayed -> reply(spot, request, delayed.value()));
    }

    private Contracts.ProbeRes reply(
        ProbeEntrySpot spot,
        Contracts.ProbeReq request,
        String value) {
        return new Contracts.ProbeRes(
            spot.context().spotRid().toString(),
            spot.context().nodeRid().toString(),
            request.op(),
            value + "#entry");
    }
}
