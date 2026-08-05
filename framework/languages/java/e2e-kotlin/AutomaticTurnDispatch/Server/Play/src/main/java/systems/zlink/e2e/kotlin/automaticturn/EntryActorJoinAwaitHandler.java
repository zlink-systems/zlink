package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActorJoinResult;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.spots.ZLinkSpotActorRequestContext;

public final class EntryActorJoinAwaitHandler {
    private final PlayEvidenceStore evidence;

    public EntryActorJoinAwaitHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @ZLinkSpotActorRequest(packetName = "ActorJoinAwaitReq")
    public CompletionStage<Contracts.ActorRes> handle(
        ProbeEntrySpot spot,
        ProbeActor actor,
        ZLinkSpotActorRequestContext context,
        Contracts.ActorJoinAwaitReq request) {
        String value = "actor=" + actor.actorId() + ";spot=" + spot.context().spotRid()
            + ";target=" + request.targetSpotRid();
        evidence.record(request.requestId(), "actor-join-await-started", value);
        evidence.record(request.requestId(), "actor-join-await-released", value);
        return actor.context()
            .joinSpot(
                RoutingId.from(request.targetSpotRid()),
                new Contracts.DelayReq(request.requestId(), 350))
            .submit()
            .thenApply(joined -> {
                String completed = value + ";joined=" + joinedActorId(joined);
                evidence.record(request.requestId(), "actor-join-await-resumed", completed);
                evidence.record(request.requestId(), "actor-join-await-completed", completed);
                return new Contracts.ActorRes(
                    "ATD-B3",
                    request.requestId(),
                    actor.actorId(),
                    "actor-join-await-completed");
            });
    }

    private static String joinedActorId(ZLinkActorJoinResult<Void> result) {
        if (result instanceof ZLinkActorJoinResult.Accepted<Void> accepted) {
            return accepted.actor().actorId();
        }
        return "rejected";
    }
}
