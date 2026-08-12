package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers

import kotlinx.coroutines.future.await
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.handlers.ZLinkSpotActorSend
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActor
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.JoinGameMsg
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.JoinGameNotify

@ZLinkHandlerGroup(SampleNames.PlayActor)
class PlayActorGetCurrentGameStateHandler {
    @ZLinkSpotActorSend
    suspend fun currentState(
        spot: TicTacToeGame,
        actor: PlayActor,
        context: ZLinkMessageContext,
        request: JoinGameMsg,
    ) {
        actor.context().boundSession()
            .send(JoinGameNotify(spot.currentState(actor, request.roomId)))
            .submit()
            .await()
    }
}
