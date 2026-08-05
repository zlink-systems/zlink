package systems.zlink.e2e.spotservice.shared;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;

public final class EntrySpotOnlyJoinHandler {
    @ZLinkSpotActorRequest(packetName = "SpotOnlyJoinReq")
    public CompletionStage<Contracts.SpotOnlyJoinRes> handle(
        ScenarioEntrySpot spot,
        ScenarioActor actor,
        ZLinkMessageContext context,
        Contracts.SpotOnlyJoinReq request) {
        if (!actor.actorId().equals(request.actorId())) {
            throw new IllegalStateException("spot-only join actor does not match dispatched actor");
        }
        actor.context()
            .joinSpot(request.targetSpotRid(), request)
            .defer();
        spot.record(
            "SpotOnlyActorJoin",
            actor.actorId() + "/" + request.targetSpotRid() + "/true/" + request.marker());
        return java.util.concurrent.CompletableFuture.completedFuture(new Contracts.SpotOnlyJoinRes(
            request.targetSpotRid(), actor.actorId(), true, request.marker()));
    }
}
