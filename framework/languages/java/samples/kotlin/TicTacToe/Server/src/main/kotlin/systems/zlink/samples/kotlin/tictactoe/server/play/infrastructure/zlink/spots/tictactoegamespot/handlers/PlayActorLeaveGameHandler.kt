package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers

import org.slf4j.LoggerFactory
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.handlers.ZLinkSpotActorSend
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActor
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.LeaveGameMsg

@ZLinkHandlerGroup(SampleNames.PlayActor)
class PlayActorLeaveGameHandler {
    @ZLinkSpotActorSend
    suspend fun leaveGame(
        spot: TicTacToeGame,
        actor: PlayActor,
        context: ZLinkMessageContext,
        message: LeaveGameMsg,
    ) {
        logger.info(
            "actor: LeaveGameMsg received. actor={}, roomId={}",
            actor.actorId,
            message.roomId,
        )
        spot.leaveGame(actor, message.roomId)
        logger.info(
            "actor: LeaveGameMsg completed. actor={}, roomId={}",
            actor.actorId,
            message.roomId,
        )
    }

    private companion object {
        val logger = LoggerFactory.getLogger(PlayActorLeaveGameHandler::class.java)
    }
}
