package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorRequestHandler
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActor
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot
import systems.zlink.samples.kotlin.bingo.shared.contracts.StopObservingBingoEventsReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.StopObservingBingoEventsRes

class StopObservingBingoEventsHandler : ZLinkSuspendingSpotActorRequestHandler<
    BingoRoomSpot,
    PlayerActor,
    StopObservingBingoEventsReq,
    StopObservingBingoEventsRes,
    > {
    override suspend fun handle(
        spot: BingoRoomSpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        request: StopObservingBingoEventsReq,
    ): StopObservingBingoEventsRes =
        spot.stopObserving(actor, request)
}
