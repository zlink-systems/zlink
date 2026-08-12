package systems.zlink.e2e.kotlin.automaticturn;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;

public final class EntryActorJoinAwaitHandler {
    private final PlayEvidenceStore evidence;

    public EntryActorJoinAwaitHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @ZLinkSpotActorRequest(packetName = "ActorJoinAwaitReq")
    public CompletionStage<Contracts.ActorRes> handle(
        ProbeEntrySpot spot,
        ProbeActor actor,
        ZLinkMessageContext context,
        Contracts.ActorJoinAwaitReq request) {
        String value = "actor=" + actor.context().actorId() + ";spot=" + spot.context().spotId()
            + ";target=" + request.targetSpotRid();
        evidence.record(request.requestId(), "actor-join-await-started", value);
        evidence.record(request.requestId(), "actor-join-await-released", value);
        actor.context()
            .joinSpot(request.targetSpotRid(),
                new Contracts.DelayReq(request.requestId(), 350))
            .timeout(Duration.ofSeconds(10))
            .defer();
        String completed = value + ";joined=deferred";
        evidence.record(request.requestId(), "actor-join-await-resumed", completed);
        evidence.record(request.requestId(), "actor-join-await-completed", completed);
        return CompletableFuture.completedFuture(new Contracts.ActorRes(
            "ATD-B3",
            request.requestId(),
            actor.context().actorId(),
            "actor-join-await-completed"));
    }
}
