package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.entryspot.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActor
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.entryspot.BingoEntrySpot
import systems.zlink.samples.kotlin.bingo.shared.contracts.ObserveBingoEventsReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.ObserveBingoEventsRes

class ObserveBingoEventsHandler : ZLinkSuspendingEntrySpotActorRequestHandler<
    BingoEntrySpot,
    PlayerActor,
    ObserveBingoEventsReq,
    ObserveBingoEventsRes,
    > {
    override suspend fun handle(
        entrySpot: BingoEntrySpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        request: ObserveBingoEventsReq,
    ): ObserveBingoEventsRes =
        entrySpot.observeEvents(actor, request)
}
