package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import systems.zlink.framework.handlers.ZLinkSpotRequest

class TimerStatusHandler {
    @ZLinkSpotRequest
    suspend fun handle(spot: TimerScenarioSpot, request: String): Contracts.TimerStatusRes =
        spot.status()
}
