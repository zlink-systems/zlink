package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers

import org.slf4j.LoggerFactory
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotTimerHandler
import systems.zlink.framework.spots.ZLinkTimerTick
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot

class BingoRoomCloseTimerHandler() : ZLinkSuspendingSpotTimerHandler<BingoRoomSpot> {
    private val logger = LoggerFactory.getLogger(BingoRoomCloseTimerHandler::class.java)

    override suspend fun handle(spot: BingoRoomSpot, tick: ZLinkTimerTick) {
        if (!spot.tryRequestClose()) return
        logger.info("bingo-lifecycle room-close spot={}", spot.context.spotId())
        spot.context.close()
    }
}
