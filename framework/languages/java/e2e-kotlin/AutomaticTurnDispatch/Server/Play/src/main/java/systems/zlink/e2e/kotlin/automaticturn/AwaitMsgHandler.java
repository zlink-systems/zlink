package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class AwaitMsgHandler
    implements ZLinkSpotPacketHandler<ProbeSpot, Contracts.AwaitMsg> {
    private final PlayEvidenceStore evidence;

    public AwaitMsgHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.AwaitMsg command) {
        String value = "spot=" + spot.context().spotRid()
            + ";correlation=" + command.correlationId()
            + ";handler=spot";
        boolean yields = command.correlationId().startsWith("TD-B");
        boolean turnContract = command.correlationId().startsWith("TD-");
        evidence.record(command.requestId(), "await-started", value);
        evidence.record(command.requestId(), yields ? "yield-released"
            : turnContract ? "await-held" : "await-released", value);
        var call = spot.context().outbound()
            .requestToChannel(
                Contracts.DELAY_CHANNEL,
                new Contracts.DelayReq(command.requestId(), command.delayMillis()))
            .timeout(Duration.ofSeconds(5));
        CompletionStage<Contracts.DelayRes> completion = yields
            ? call.yield(Contracts.DelayRes.class)
            : call.submit(Contracts.DelayRes.class);
        return completion
            .thenAccept(reply -> {
                evidence.record(command.requestId(), yields ? "yield-resumed" : "await-resumed", value);
                evidence.record(command.requestId(), turnContract ? "completed" : "await-completed", value);
            });
    }
}
