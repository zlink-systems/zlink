package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers;

import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame;

import java.util.concurrent.CompletionStage;

// --8<-- [start:doc-timer-handler]
public final class TicTacToeGameTimerHandler implements ZLinkSpotTimerHandler<TicTacToeGame> {
    @Override
    public CompletionStage<Void> handle(TicTacToeGame spot, ZLinkTimerTick tick) {
        return spot.tick();
    }
}
// --8<-- [end:doc-timer-handler]
