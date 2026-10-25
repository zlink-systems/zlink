package MyGame.field;

import MyGame.*;
import systems.zlink.framework.handlers.ZLinkSpotRequest;

import java.util.concurrent.CompletionStage;

public final class PartyRequests {
    @ZLinkSpotRequest
    public CompletionStage<PartyCommandResult> command(PartySpot spot, PartyCommand request) {
        return spot.command(request);
    }

    @ZLinkSpotRequest
    public CompletionStage<PartyCommandResult> memberEntered(PartySpot spot, DungeonMemberEntered request) {
        return spot.progress(request.runId(), request.actorId(), false);
    }

    @ZLinkSpotRequest
    public CompletionStage<PartyCommandResult> started(PartySpot spot, RunStarted request) {
        return spot.progress(request.runId(), null, false);
    }

    @ZLinkSpotRequest
    public CompletionStage<PartyCommandResult> completed(PartySpot spot, RunCompleted request) {
        return spot.progress(request.runId(), null, true);
    }
}
