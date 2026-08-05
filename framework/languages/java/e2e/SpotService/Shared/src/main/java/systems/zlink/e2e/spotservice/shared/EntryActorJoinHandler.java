package systems.zlink.e2e.spotservice.shared;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;

public final class EntryActorJoinHandler {
    @ZLinkSpotActorRequest(packetName = "ActorJoinReq")
    public CompletionStage<Contracts.ActorJoinRes> handle(
        ScenarioEntrySpot spot,
        ScenarioActor actor,
        ZLinkMessageContext context,
        Contracts.ActorJoinReq request) {
        actor.applyProfile(request.profile());
        spot.record("ActorJoinPayload", payloadEvidence(request));
        actor.context()
            .joinSpot(request.spotRid(), request)
            .defer();
        return java.util.concurrent.CompletableFuture.completedFuture(new Contracts.ActorJoinRes(
            actor.actorId(),
            request.spotRid(),
            spot.nodeRid(),
            request.profile().displayName(),
            request.profile().level(),
            request.tags()));
    }

    private static String payloadEvidence(Contracts.ActorJoinReq request) {
        return request.profile().displayName()
            + "/" + request.profile().level()
            + "/" + String.join(",", request.tags());
    }
}
