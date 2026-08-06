package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletableFuture;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;

public final class EntryActorJoinHandler {
    @ZLinkSpotActorRequest(packetName = "ActorJoinReq")
    public CompletionStage<Contracts.ActorJoinRes> handle(
        ProbeEntrySpot spot,
        ProbeActor actor,
        ZLinkMessageContext context,
        Contracts.ActorJoinReq request) {
        actor.context()
            .joinSpot(request.spotRid(), request)
            .defer();
        return CompletableFuture.completedFuture(new Contracts.ActorJoinRes(
                actor.context().actorId(),
                request.spotRid(),
                "join-deferred:" + request.value()));
    }
}
