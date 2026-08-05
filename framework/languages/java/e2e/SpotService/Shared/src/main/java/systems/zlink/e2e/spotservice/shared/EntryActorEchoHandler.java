package systems.zlink.e2e.spotservice.shared;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;

public final class EntryActorEchoHandler {
    @ZLinkSpotActorRequest(packetName = "ActorEchoReq")
    public CompletionStage<Contracts.ActorEchoRes> handle(
        ScenarioEntrySpot spot,
        ScenarioActor actor,
        ZLinkMessageContext context,
        Contracts.ActorEchoReq request) {
        int seq = actor.nextSequence();
        spot.record("ActorEntryReq", actor.actorId() + "/" + request.value() + "#" + seq);
        actor.context().boundSession()
            .send(new Contracts.ActorPushNotify(actor.actorId(), "entry", "push:" + request.value(), request.seq(), seq))
            .submit();
        return CompletableFuture.completedFuture(new Contracts.ActorEchoRes(
            actor.actorId(),
            "entry",
            spot.nodeRid(),
            "entry:" + request.value(),
            request.seq(),
            seq,
            request.profile().displayName(),
            request.profile().level(),
            request.profile().tags()));
    }
}
