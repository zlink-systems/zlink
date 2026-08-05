package systems.zlink.samples.kotlin.bingo.server.matchmaking

import systems.zlink.framework.kotlin.ZLinkSuspendingSpotTimerHandler
import systems.zlink.framework.spots.ZLinkTimerTick

class BingoMatchmakerIdleTimerHandler : ZLinkSuspendingSpotTimerHandler<BingoMatchmaker> {
    override suspend fun handle(
        spot: BingoMatchmaker,
        tick: ZLinkTimerTick,
    ) {
        spot.closeIfIdle()
    }
}
