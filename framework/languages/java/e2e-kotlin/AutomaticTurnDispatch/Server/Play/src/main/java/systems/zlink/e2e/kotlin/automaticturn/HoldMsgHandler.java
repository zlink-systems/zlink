package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class HoldMsgHandler
    implements ZLinkSpotPacketHandler<ProbeSpot, Contracts.HoldMsg> {
    private final PlayEvidenceStore evidence;

    public HoldMsgHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.HoldMsg command) {
        String value = "spot=" + spot.context().spotRid()
            + ";handler=spot";
        evidence.record(command.requestId(), "hold-started", value);
        return spot.context().outbound()
            .requestToChannel(
                Contracts.DELAY_CHANNEL,
                new Contracts.DelayReq(command.requestId(), command.delayMillis()))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.DelayRes.class)
            .thenAccept(reply -> {
                evidence.record(command.requestId(), "hold-resumed", value);
                evidence.record(command.requestId(), "hold-completed", value);
            });
    }
}
