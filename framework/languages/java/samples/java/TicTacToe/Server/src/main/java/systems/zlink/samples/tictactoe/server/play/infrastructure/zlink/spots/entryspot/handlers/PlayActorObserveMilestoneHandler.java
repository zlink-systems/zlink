package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers;

import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors.PlayActor;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot.PlayEntrySpot;
import systems.zlink.samples.tictactoe.shared.contracts.ObserveMilestoneReq;
import systems.zlink.samples.tictactoe.shared.contracts.ObserveMilestoneRes;

@ZLinkHandlerGroup(SampleNames.PlayActor)
public final class PlayActorObserveMilestoneHandler {
    @ZLinkSpotActorRequest
    public java.util.concurrent.CompletionStage<ObserveMilestoneRes> observe(
        PlayEntrySpot entrySpot,
        PlayActor actor,
        ZLinkMessageContext context,
        ObserveMilestoneReq request) {
        return java.util.concurrent.CompletableFuture.completedFuture(entrySpot.observeMilestone(actor));
    }
}
