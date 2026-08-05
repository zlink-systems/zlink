package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class CleanupProbeReqHandler {
    private final PlayEvidenceStore evidence;

    public CleanupProbeReqHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @ZLinkSpotRequest
    public CompletionStage<Contracts.CleanupProbeRes> handle(
        ProbeSpot spot,
        Contracts.CleanupProbeReq request) {
        String value = "spot=" + spot.context().spotRid()
            + ";node=" + spot.context().nodeRid()
            + ";marker=" + request.marker();
        evidence.record(request.requestId(), "probe-started", value);
        evidence.record(request.requestId(), "probe-completed", value);
        return CompletableFuture.completedFuture(
            new Contracts.CleanupProbeRes(request.requestId()));
    }
}
