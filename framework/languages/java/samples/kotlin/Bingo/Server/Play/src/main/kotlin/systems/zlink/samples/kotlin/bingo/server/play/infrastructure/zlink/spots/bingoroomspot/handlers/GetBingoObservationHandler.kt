package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.spots.ZLinkSpotRequestHandler
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot
import systems.zlink.samples.kotlin.bingo.shared.contracts.Messages

class GetBingoObservationHandler :
    ZLinkSpotRequestHandler<
        BingoRoomSpot,
        Messages.GetBingoObservationReq,
        Messages.GetBingoObservationRes,
    > {
    override fun handle(
        spot: BingoRoomSpot,
        request: Messages.GetBingoObservationReq,
    ): CompletionStage<Messages.GetBingoObservationRes> =
        CompletableFuture.completedFuture(spot.observation())
}
