package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.spots.ZLinkSpotActorRequestContext;

public final class EntryActorAwaitHandler {
    private final PlayEvidenceStore evidence;

    public EntryActorAwaitHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @ZLinkSpotActorRequest(packetName = "ActorAwaitReq")
    public CompletionStage<Contracts.ActorRes> handle(
        ProbeEntrySpot spot,
        ProbeActor actor,
        ZLinkSpotActorRequestContext context,
        Contracts.ActorAwaitReq request) {
        String value = "actor=" + actor.actorId() + ";spot=" + spot.context().spotRid();
        evidence.record(request.requestId(), "actor-await-started", value);
        evidence.record(request.requestId(), "actor-await-released", value);
        return spot.context().outbound()
            .requestToChannel(
                Contracts.DELAY_CHANNEL,
                new Contracts.DelayReq(request.requestId(), request.delayMillis()))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.DelayRes.class)
            .thenApply(reply -> {
                evidence.record(request.requestId(), "actor-await-resumed", value);
                evidence.record(request.requestId(), "actor-await-completed", value);
                return new Contracts.ActorRes(
                    "ATD-B", request.requestId(), actor.actorId(), "actor-await-completed");
            });
    }
}
