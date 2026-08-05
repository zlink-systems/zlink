package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.entryspot.handlers

import kotlinx.coroutines.future.await
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorSendHandler
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActor
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.entryspot.BingoEntrySpot
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomJoinReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoApiReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoApiRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoReq

class MatchBingoActorHandler : ZLinkSuspendingEntrySpotActorSendHandler<
    BingoEntrySpot,
    PlayerActor,
    MatchBingoReq,
    > {
    override suspend fun handle(
        entrySpot: BingoEntrySpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        message: MatchBingoReq,
    ) {
        val matched = entrySpot.context().outbound().requestToChannel(
            SampleNames.ApiChannel,
            MatchBingoApiReq(
                actor.actorId(),
                actor.displayName,
                message.mode,
            ),
        )
            .timeout(SampleTimings.RequestTimeout)
            .submit(MatchBingoApiRes::class.java)
            .await()
        actor.trackDeferredJoin(matched.roomId)
        actor.context().joinSpot(
            matched.roomId,
            BingoRoomJoinReq(
                matched.roomId,
                actor.actorId(),
                actor.displayName,
                false,
            ),
        )
            .timeout(SampleTimings.RequestTimeout)
            .defer()
    }
}
