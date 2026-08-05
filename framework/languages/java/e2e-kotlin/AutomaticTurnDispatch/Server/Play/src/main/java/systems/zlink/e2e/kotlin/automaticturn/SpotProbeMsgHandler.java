package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class SpotProbeMsgHandler
    implements ZLinkSpotPacketHandler<ProbeSpot, Contracts.SpotProbeMsg> {
    private final PlayEvidenceStore evidence;

    public SpotProbeMsgHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.SpotProbeMsg command) {
        String value = "spot=" + spot.context().spotRid()
            + ";node=" + spot.context().nodeRid()
            + ";marker=" + command.marker();
        evidence.record(command.requestId(), "probe-completed", value);
        return CompletableFuture.completedFuture(null);
    }
}
