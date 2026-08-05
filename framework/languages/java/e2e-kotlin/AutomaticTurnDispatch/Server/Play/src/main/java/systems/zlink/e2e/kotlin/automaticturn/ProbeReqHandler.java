package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class ProbeReqHandler {
    @ZLinkSpotRequest
    public CompletionStage<Contracts.ProbeRes> handle(
        ProbeSpot spot,
        Contracts.ProbeReq request) {
        return spot.handle(request);
    }
}
