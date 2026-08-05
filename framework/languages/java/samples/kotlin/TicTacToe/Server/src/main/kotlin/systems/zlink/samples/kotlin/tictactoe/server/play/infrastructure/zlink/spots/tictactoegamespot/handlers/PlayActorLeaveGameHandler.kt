package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers

import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.handlers.ZLinkSpotActorSend
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActor
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.LeaveGameReq

@ZLinkHandlerGroup(SampleNames.PlayActor)
class PlayActorLeaveGameHandler {
    @ZLinkSpotActorSend
    suspend fun leaveGame(
        spot: TicTacToeGame,
        actor: PlayActor,
        context: ZLinkMessageContext,
        request: LeaveGameReq,
    ) {
        spot.leaveGame(actor, request.roomId)
    }
}
