package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class ShutdownAwaitReqHandler {
    private final PlayEvidenceStore evidence;

    public ShutdownAwaitReqHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @ZLinkSpotRequest
    public CompletionStage<Contracts.ScenarioRes> handle(
        ProbeSpot spot,
        Contracts.ShutdownAwaitReq request) {
        String value = "spot=" + spot.context().spotRid() + ";scenario=ATD-E3";
        evidence.record(request.requestId(), "shutdown-await-started", value);
        evidence.record(request.requestId(), "shutdown-await-released", value);
        return spot.context().outbound()
            .requestToChannel(
                Contracts.DELAY_CHANNEL,
                new Contracts.DelayReq(request.requestId(), request.delayMillis()))
            .timeout(Duration.ofSeconds(10))
            .submit(Contracts.DelayRes.class)
            .thenApply(reply -> {
                evidence.record(request.requestId(), "shutdown-await-resumed", value);
                evidence.record(request.requestId(), "shutdown-await-completed", value);
                return new Contracts.ScenarioRes(
                    "ATD-E3",
                    request.requestId(),
                    spot.context().nodeRid().toString());
            });
    }
}
