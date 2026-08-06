package systems.zlink.e2e.observabilityops.play;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.automaticturn.shared.AwaitActor;
import systems.zlink.e2e.automaticturn.shared.Contracts;
import systems.zlink.e2e.automaticturn.shared.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;

public final class ObservabilityActorJoinHandler implements
    ZLinkEntrySpotActorRequestHandler<ObservabilityEntrySpot, AwaitActor,
        Contracts.ActorJoinReq, Contracts.ActorJoinRes> {
    private final EvidenceStore evidence;

    public ObservabilityActorJoinHandler(EvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Contracts.ActorJoinRes> handle(
        ObservabilityEntrySpot spot,
        AwaitActor actor,
        ZLinkMessageContext context,
        Contracts.ActorJoinReq request) {
        actor.context().joinSpot(request.spotRid(), "join")
            .timeout(Duration.ofSeconds(5))
            .defer();
        evidence.record("actor-joined", request.requestId(),
            "actor=" + actor.actorId() + ";spot=" + request.spotRid());
        return CompletableFuture.completedFuture(new Contracts.ActorJoinRes(
            "ATD-B-JOIN", request.requestId(), actor.actorId(), "joined"));
    }
}
