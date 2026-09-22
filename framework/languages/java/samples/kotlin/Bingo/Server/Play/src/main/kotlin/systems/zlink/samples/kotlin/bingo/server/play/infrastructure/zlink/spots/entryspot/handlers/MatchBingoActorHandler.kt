package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.entryspot.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToChannel
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActor
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.entryspot.BingoEntrySpot
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoPlayerState
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomJoinReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomState
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoApiReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoApiRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoRes

class MatchBingoActorHandler(routes: ZLinkRouteClient) :
    ZLinkSuspendingEntrySpotActorRequestHandler<
        BingoEntrySpot,
        PlayerActor,
        MatchBingoReq,
        MatchBingoRes,
    > {
    private val kotlinRoutes = routes.kotlin()

    override suspend fun handle(
        entrySpot: BingoEntrySpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        message: MatchBingoReq,
    ): MatchBingoRes {
        // --8<-- [start:doc-bingo-match-actor]
        val matched =
            kotlinRoutes
                .requestToChannel<MatchBingoApiRes>(
                    SampleNames.ApiChannel,
                    MatchBingoApiReq(actor.actorId(), actor.displayName, message.mode),
                )
                .timeout(SampleTimings.RequestTimeout)
                .await()
        actor.trackDeferredJoin(matched.roomId)
        actor
            .context()
            .joinSpot(
                matched.roomId,
                BingoRoomJoinReq(matched.roomId, actor.actorId(), actor.displayName, false),
            )
            .timeout(SampleTimings.RequestTimeout)
            .defer()
        // --8<-- [end:doc-bingo-match-actor]
        return MatchBingoRes(
            matched.roomId,
            BingoRoomState(
                roomId = matched.roomId,
                status = "WaitingForPlayers",
                hostActorId = actor.actorId(),
                canStart = false,
                drawSeq = 0,
                lastDrawnNumber = null,
                drawnNumbers = emptyList(),
                players =
                    listOf(
                        BingoPlayerState(
                            actorId = actor.actorId(),
                            displayName = actor.displayName,
                            seat = 1,
                            isHost = true,
                            card = emptyList(),
                            marks = emptyList(),
                            completedLines = 0,
                            wins = 0,
                            losses = 0,
                        )
                    ),
                winners = emptyList(),
            ),
        )
    }
}
