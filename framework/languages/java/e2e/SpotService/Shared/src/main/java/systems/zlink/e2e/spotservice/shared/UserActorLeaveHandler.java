package systems.zlink.e2e.spotservice.shared;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;

public final class UserActorLeaveHandler {
    @ZLinkSpotActorRequest(packetName = "LeaveActorReq")
    public CompletionStage<Contracts.LeaveActorRes> handle(
        UserSpot spot,
        ScenarioActor actor,
        ZLinkMessageContext context,
        Contracts.LeaveActorReq request) {
        if (!request.actorId().equals(actor.actorId())) {
            throw new IllegalStateException("leave request actor does not match dispatched actor");
        }
        return spot.context().leaveActor(actor)
            .thenApply(ignored -> new Contracts.LeaveActorRes(actor.actorId(), true));
    }
}
