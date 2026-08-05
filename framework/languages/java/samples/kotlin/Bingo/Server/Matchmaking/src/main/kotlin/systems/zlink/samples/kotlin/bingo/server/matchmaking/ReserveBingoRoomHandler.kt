package systems.zlink.samples.kotlin.bingo.server.matchmaking

import java.util.concurrent.CompletionStage
import systems.zlink.framework.spots.ZLinkSpotRequestHandler
import systems.zlink.samples.kotlin.bingo.shared.contracts.ReserveBingoRoomReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.ReserveBingoRoomRes

class ReserveBingoRoomHandler(
    private val reservations: RedisBingoMatchReservationStore,
) : ZLinkSpotRequestHandler<BingoMatchmaker, ReserveBingoRoomReq, ReserveBingoRoomRes> {
    override fun handle(
        spot: BingoMatchmaker,
        request: ReserveBingoRoomReq,
    ): CompletionStage<ReserveBingoRoomRes> {
        spot.beginRequest()
        return reservations.reserve(request)
            .whenComplete { _, _ -> spot.endRequest() }
    }
}
