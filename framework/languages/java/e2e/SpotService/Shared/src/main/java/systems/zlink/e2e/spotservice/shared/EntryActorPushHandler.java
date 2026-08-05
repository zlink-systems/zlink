package systems.zlink.e2e.spotservice.shared;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;

public final class EntryActorPushHandler {
    @ZLinkSpotActorRequest(packetName = "ActorPushReq")
    public CompletionStage<Contracts.ActorPingRes> handle(
        ScenarioEntrySpot spot,
        ScenarioActor actor,
        ZLinkMessageContext context,
        Contracts.ActorPushReq request) {
        int seq = actor.nextSequence();
        spot.record("ActorPushReq", actor.actorId() + "/" + request.value() + "#" + seq);
        actor.context().boundSession()
            .send(new Contracts.ActorPushNotify(actor.actorId(), "entry", request.value(), seq, seq))
            .submit();
        return CompletableFuture.completedFuture(new Contracts.ActorPingRes(
            actor.actorId(),
            spot.nodeRid(),
            "entry",
            request.value(),
            seq));
    }
}
