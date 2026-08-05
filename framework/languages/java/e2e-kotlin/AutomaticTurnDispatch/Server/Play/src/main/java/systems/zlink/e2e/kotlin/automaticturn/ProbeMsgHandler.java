package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class ProbeMsgHandler
    implements ZLinkSpotPacketHandler<ProbeSpot, Contracts.ProbeMsg> {
    private final PlayEvidenceStore evidence;

    public ProbeMsgHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.ProbeMsg command) {
        String value = "spot=" + spot.context().spotRid()
            + ";node=" + spot.context().nodeRid()
            + ";marker=" + command.marker();
        evidence.record(command.requestId(), "probe-started", value);
        evidence.record(command.requestId(), "probe-completed", value);
        return CompletableFuture.completedFuture(null);
    }
}
