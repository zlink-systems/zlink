package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers

import systems.zlink.framework.kotlin.ZLinkSuspendingSpotTimerHandler
import systems.zlink.framework.spots.ZLinkTimerTick
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame

// --8<-- [start:doc-timer-handler]
class TicTacToeGameTimerHandler() : ZLinkSuspendingSpotTimerHandler<TicTacToeGame> {
    override suspend fun handle(
        spot: TicTacToeGame,
        tick: ZLinkTimerTick,
    ) = run {
        spot.tick()
    }
}
// --8<-- [end:doc-timer-handler]
