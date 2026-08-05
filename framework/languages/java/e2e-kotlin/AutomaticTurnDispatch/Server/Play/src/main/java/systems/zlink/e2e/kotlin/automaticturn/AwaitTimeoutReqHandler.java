package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

public final class AwaitTimeoutReqHandler
    implements ZLinkSpotRequestHandler<ProbeSpot, Contracts.AwaitTimeoutReq, Contracts.AwaitTimeoutRes> {
    private final PlayEvidenceStore evidence;

    public AwaitTimeoutReqHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Contracts.AwaitTimeoutRes> handle(
        ProbeSpot spot,
        Contracts.AwaitTimeoutReq request) {
        String value = "spot=" + spot.context().spotRid() + ";node=" + spot.context().nodeRid();
        evidence.record(request.requestId(), "timeout-await-started", value);
        evidence.record(request.requestId(), "timeout-await-released", value);
        return spot.context().outbound()
            .requestToChannel(
                Contracts.DELAY_CHANNEL,
                new Contracts.DelayReq(request.requestId(), request.delayMillis()))
            .timeout(Duration.ofMillis(request.timeoutMillis()))
            .submit(Contracts.DelayRes.class)
            .handle((reply, error) -> {
                boolean timedOut = error != null;
                String errorType = timedOut ? error.getClass().getSimpleName() : "";
                if (timedOut) {
                    evidence.record(request.requestId(), "timeout-await-completed", value + ";error=" + errorType);
                } else {
                    evidence.record(request.requestId(), "timeout-await-unexpected-resumed", value);
                }
                return new Contracts.AwaitTimeoutRes(
                    "ATD-E1",
                    request.requestId(),
                    spot.context().spotRid().toString(),
                    spot.context().nodeRid().toString(),
                    timedOut,
                    errorType);
            });
    }
}
