package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class AwaitReqHandler {
    private final PlayEvidenceStore evidence;

    public AwaitReqHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @ZLinkSpotRequest
    public CompletionStage<Contracts.ScenarioRes> handle(
        ProbeSpot spot,
        Contracts.AwaitReq request) {
        String value = "spot=" + spot.context().spotRid() + ";correlation=" + request.correlationId();
        evidence.record(request.requestId(), "await-started", value);
        evidence.record(request.requestId(), "await-released", value);
        return spot.context().outbound()
            .requestToChannel(
                Contracts.DELAY_CHANNEL,
                new Contracts.DelayReq(request.requestId(), 350))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.DelayRes.class)
            .thenApply(reply -> {
                evidence.record(request.requestId(), "await-resumed", value);
                evidence.record(request.requestId(), "await-completed", value);
                return new Contracts.ScenarioRes(
                    request.scenarioId(),
                    request.requestId(),
                    spot.context().nodeRid().toString());
            });
    }
}
