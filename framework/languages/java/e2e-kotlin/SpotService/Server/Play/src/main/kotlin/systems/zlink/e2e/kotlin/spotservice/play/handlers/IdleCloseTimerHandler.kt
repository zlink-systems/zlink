package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotTimerHandler
import systems.zlink.framework.spots.ZLinkTimerTick

class IdleCloseTimerHandler : ZLinkSuspendingSpotTimerHandler<TimerScenarioSpot> {
    override suspend fun handle(spot: TimerScenarioSpot, tick: ZLinkTimerTick) {
        spot.idleTick()
    }
}
