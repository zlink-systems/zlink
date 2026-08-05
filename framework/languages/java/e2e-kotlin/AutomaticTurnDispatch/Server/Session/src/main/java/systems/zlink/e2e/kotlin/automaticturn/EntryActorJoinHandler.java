package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.spots.ZLinkSpotActorRequestContext;

public final class EntryActorJoinHandler {
    @ZLinkSpotActorRequest(packetName = "ActorJoinReq")
    public CompletionStage<Contracts.ActorJoinRes> handle(
        ProbeEntrySpot spot,
        ProbeActor actor,
        ZLinkSpotActorRequestContext context,
        Contracts.ActorJoinReq request) {
        return actor.context()
            .joinSpot(RoutingId.from(request.spotRid()), request)
            .submit(Contracts.ActorJoinRes.class)
            .thenApply(joined -> new Contracts.ActorJoinRes(
                actor.actorId(), request.spotRid(), "joined:" + request.value()));
    }
}
