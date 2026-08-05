package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.handlers.ZLinkSpotRequest;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.spots.SpotHandleResolver;

public final class RemoteSpotAwaitReqHandler {
    private final PlayEvidenceStore evidence;
    private final SpotHandleResolver spots;

    public RemoteSpotAwaitReqHandler(PlayEvidenceStore evidence, SpotHandleResolver spots) {
        this.evidence = evidence;
        this.spots = spots;
    }

    @ZLinkSpotRequest
    public CompletionStage<Contracts.ScenarioRes> handle(
        ProbeSpot spot,
        Contracts.RemoteSpotAwaitReq request) {
        String value = "spot=" + spot.context().spotRid() + ";target=" + request.targetSpotRid();
        evidence.record(request.requestId(), "remote-await-started", value);
        evidence.record(request.requestId(), "remote-await-released", value);
        RoutingId targetSpotRid = RoutingId.from(request.targetSpotRid());
        return spots.resolveSpotHandle(targetSpotRid)
            .thenCompose(target -> spot.context().outbound()
                .requestToSpot(
                    requireSpot(target, targetSpotRid),
                    new Contracts.AwaitReq("ATD-D2", request.requestId(), "remote-spot"))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.ScenarioRes.class))
            .thenApply(targetReply -> {
                String resumed = value + ";targetNode=" + targetReply.result();
                evidence.record(request.requestId(), "remote-await-resumed", resumed);
                evidence.record(request.requestId(), "remote-await-completed", resumed);
                return new Contracts.ScenarioRes(
                    "ATD-D2", request.requestId(), spot.context().nodeRid().toString());
            });
    }

    private static SpotHandle requireSpot(Optional<SpotHandle> handle, RoutingId spotRid) {
        return handle.orElseThrow(() -> new IllegalStateException("spot not found: " + spotRid));
    }
}
