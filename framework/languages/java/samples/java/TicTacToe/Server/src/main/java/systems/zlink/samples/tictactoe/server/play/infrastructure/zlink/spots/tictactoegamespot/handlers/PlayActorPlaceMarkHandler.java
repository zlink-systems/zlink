package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors.PlayActor;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame;
import systems.zlink.samples.tictactoe.shared.contracts.PlaceMarkReq;
import systems.zlink.samples.tictactoe.shared.contracts.PlaceMarkRes;

@ZLinkHandlerGroup("play-actor")
// --8<-- [start:doc-actor-packet-handler]
public final class PlayActorPlaceMarkHandler {
    @ZLinkSpotActorRequest
    public CompletionStage<PlaceMarkRes> placeMark(
        TicTacToeGame spot,
        PlayActor actor,
        ZLinkMessageContext context,
        PlaceMarkReq request) {
        actor.requireJoinedGame();
        PlaceMarkRes response = spot.placeMark(actor, request.cell());
        return CompletableFuture.completedFuture(response);
    }
}
// --8<-- [end:doc-actor-packet-handler]
