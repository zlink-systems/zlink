package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class AwaitTimeoutMsgHandler
    implements ZLinkSpotPacketHandler<ProbeSpot, Contracts.AwaitTimeoutMsg> {
    private final PlayEvidenceStore evidence;

    public AwaitTimeoutMsgHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.AwaitTimeoutMsg command) {
        String value = "spot=" + spot.context().spotRid() + ";node=" + spot.context().nodeRid();
        evidence.record(command.requestId(), "timeout-await-started", value);
        evidence.record(command.requestId(), "timeout-await-released", value);
        return spot.context().outbound()
            .requestToChannel(
                Contracts.DELAY_CHANNEL,
                new Contracts.DelayReq(command.requestId(), command.delayMillis()))
            .timeout(Duration.ofMillis(command.timeoutMillis()))
            .submit(Contracts.DelayRes.class)
            .handle((reply, error) -> {
                if (error == null) {
                    evidence.record(command.requestId(), "timeout-await-unexpected-resumed", value);
                } else {
                    evidence.record(command.requestId(), "timeout-await-completed", value + ";error=" + error);
                }
                return null;
            });
    }
}
