package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import kotlinx.coroutines.delay
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotTimerHandler
import systems.zlink.framework.spots.ZLinkTimerTick

class TimerOverrunHandler : ZLinkSuspendingSpotTimerHandler<TimerScenarioSpot> {
    override suspend fun handle(spot: TimerScenarioSpot, tick: ZLinkTimerTick) {
        delay(160)
        spot.overrunTick(tick.deliveryIndex(), tick.skippedTicks())
    }
}
