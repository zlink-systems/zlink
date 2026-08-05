package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.handlers.ZLinkSpotActorSend
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActor
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot.PlayEntrySpot
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.JoinGameReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.TicTacToeGameJoinReq

@ZLinkHandlerGroup(SampleNames.PlayActor)
// --8<-- [start:doc-join-defer]
class PlayActorJoinGameHandler {
    @ZLinkSpotActorSend
    suspend fun joinGame(
        entrySpot: PlayEntrySpot,
        actor: PlayActor,
        context: ZLinkMessageContext,
        request: JoinGameReq,
    ) {
        actor.trackDeferredJoin(request.roomId)
        actor.context()
            .joinSpot(request.roomId, TicTacToeGameJoinReq(request.roomId, actor.requirePlayer()))
            .timeout(SampleNames.RequestTimeout)
            .defer()
    }
}
// --8<-- [end:doc-join-defer]
