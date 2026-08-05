package systems.zlink.e2e.spotservice.shared;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;

public final class EntryActorSnapshotHandler {
    @ZLinkSpotActorRequest(packetName = "SnapshotReq")
    public CompletionStage<Contracts.SnapshotRes> handle(
        ScenarioEntrySpot spot,
        ScenarioActor actor,
        ZLinkMessageContext context,
        Contracts.SnapshotReq request) {
        if (!actor.actorId().equals(request.actorId())) {
            throw new IllegalStateException("Snapshot request actor does not match dispatched actor.");
        }
        return CompletableFuture.completedFuture(
            new Contracts.SnapshotRes(actor.actorId(), actor.currentSequence()));
    }
}
