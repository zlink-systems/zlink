package systems.zlink.e2e.spotservice.shared;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;

public final class EntryActorPingHandler {
    @ZLinkSpotActorRequest(packetName = "ActorPingReq")
    public CompletionStage<Contracts.ActorPingRes> handle(
        ScenarioEntrySpot spot,
        ScenarioActor actor,
        ZLinkMessageContext context,
        Contracts.ActorPingReq request) {
        int seq = actor.nextSequence();
        spot.record("ActorPingReq", actor.actorId() + "/" + request.value() + "#" + seq);
        return CompletableFuture.completedFuture(new Contracts.ActorPingRes(
            actor.actorId(),
            spot.nodeRid(),
            "entry",
            request.value(),
            seq));
    }
}
