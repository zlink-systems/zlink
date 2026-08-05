package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.spots.ZLinkSpotActorRequestContext;

public final class ProbeActorRequestHandler {
    @ZLinkSpotActorRequest(packetName = "ProbeReq")
    public CompletionStage<Contracts.ProbeRes> handle(
        ProbeSpot spot,
        ProbeActor actor,
        ZLinkSpotActorRequestContext context,
        Contracts.ProbeReq request) {
        return spot.handle(request);
    }
}
