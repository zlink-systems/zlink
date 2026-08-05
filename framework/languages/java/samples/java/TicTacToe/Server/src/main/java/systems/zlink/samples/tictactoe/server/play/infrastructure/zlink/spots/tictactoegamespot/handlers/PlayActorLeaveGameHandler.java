package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers;

import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors.PlayActor;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame;
import systems.zlink.samples.tictactoe.shared.contracts.LeaveGameReq;

@ZLinkHandlerGroup(SampleNames.PlayActor)
public final class PlayActorLeaveGameHandler {
    @ZLinkSpotActorSend
    public java.util.concurrent.CompletionStage<Void> leaveGame(
        TicTacToeGame spot,
        PlayActor actor,
        ZLinkMessageContext context,
        LeaveGameReq request) {
        return spot.leaveGame(actor, request.roomId());
    }
}
