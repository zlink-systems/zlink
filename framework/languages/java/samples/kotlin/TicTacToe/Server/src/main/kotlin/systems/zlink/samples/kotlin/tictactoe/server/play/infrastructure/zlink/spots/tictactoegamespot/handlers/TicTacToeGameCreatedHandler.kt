package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers

import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.TicTacToeGameCreateReq

class TicTacToeGameCreatedHandler {
    fun handle(
        game: TicTacToeGame,
        request: ZLinkMessage,
    ) {
        game.markCreated(request.decode(TicTacToeGameCreateReq::class.java))
    }
}
