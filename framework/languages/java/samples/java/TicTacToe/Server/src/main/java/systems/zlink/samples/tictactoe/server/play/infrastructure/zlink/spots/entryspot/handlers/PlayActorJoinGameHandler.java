package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors.PlayActor;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot.PlayEntrySpot;
import systems.zlink.samples.tictactoe.shared.contracts.JoinGameReq;
import systems.zlink.samples.tictactoe.shared.contracts.TicTacToeGameJoinReq;

@ZLinkHandlerGroup(SampleNames.PlayActor)
// --8<-- [start:doc-join-defer]
public final class PlayActorJoinGameHandler {
    @ZLinkSpotActorSend
    public java.util.concurrent.CompletionStage<Void> joinGame(
        PlayEntrySpot entrySpot,
        PlayActor actor,
        ZLinkMessageContext context,
        JoinGameReq request) {
        actor.trackDeferredJoin(request.roomId());
        actor.context()
            .joinSpot(request.roomId(),
                new TicTacToeGameJoinReq(request.roomId(), actor.requirePlayer()))
            .timeout(SampleNames.RequestTimeout)
            .defer();
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
// --8<-- [end:doc-join-defer]
