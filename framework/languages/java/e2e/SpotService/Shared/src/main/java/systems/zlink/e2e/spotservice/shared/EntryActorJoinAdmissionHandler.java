package systems.zlink.e2e.spotservice.shared;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;

public final class EntryActorJoinAdmissionHandler {
    @ZLinkSpotActorRequest(packetName = "JoinAdmittedUserSpotActorReq")
    public CompletionStage<Contracts.JoinAdmittedUserSpotActorRes> handle(
        ScenarioEntrySpot spot,
        ScenarioActor actor,
        ZLinkMessageContext context,
        Contracts.JoinAdmittedUserSpotActorReq request) {
        actor.applyProfile(request.profile());
        actor.context()
            .joinSpot(request.spotRid(), request)
            .defer();
        return java.util.concurrent.CompletableFuture.completedFuture(
            new Contracts.JoinAdmittedUserSpotActorRes(
                actor.actorId(),
                request.spotRid(),
                spot.nodeRid(),
                request.admit(),
                request.admit() ? "" : "ActorJoinRejected"));
    }
}
