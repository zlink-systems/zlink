package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.spots.ZLinkSpotActorRequestContext;

public final class EntryActorPushAwaitHandler {
    private final PlayEvidenceStore evidence;

    public EntryActorPushAwaitHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @ZLinkSpotActorRequest(packetName = "ActorPushAwaitReq")
    public CompletionStage<Contracts.ActorRes> handle(
        ProbeEntrySpot spot,
        ProbeActor actor,
        ZLinkSpotActorRequestContext context,
        Contracts.ActorPushAwaitReq request) {
        String value = "actor=" + actor.actorId() + ";spot=" + spot.context().spotRid();
        evidence.record(request.requestId(), "actor-push-await-started", value);
        evidence.record(request.requestId(), "actor-push-await-released", value);
        return spot.context().outbound()
            .requestToChannel(
                Contracts.DELAY_CHANNEL,
                new Contracts.DelayReq(request.requestId(), request.delayMillis()))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.DelayRes.class)
            .thenApply(reply -> {
                evidence.record(request.requestId(), "actor-push-await-resumed", value);
                actor.context().boundSession()
                    .send(new Contracts.ActorPushNotify(
                        actor.actorId(),
                        request.requestId(),
                        request.value(),
                        spot.context().nodeRid().toString()))
                    .submit();
                evidence.record(request.requestId(), "actor-push-await-completed", value);
                return new Contracts.ActorRes(
                    "ATD-D4", request.requestId(), actor.actorId(), "actor-push-await-completed");
            });
    }
}
