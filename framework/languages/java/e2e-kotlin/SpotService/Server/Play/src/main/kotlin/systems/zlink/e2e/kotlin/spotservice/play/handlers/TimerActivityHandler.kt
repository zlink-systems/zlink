package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import systems.zlink.framework.handlers.ZLinkSpotRequest

class TimerActivityHandler {
    @ZLinkSpotRequest
    suspend fun handle(
        spot: TimerScenarioSpot,
        request: Contracts.TimerActivityReq,
    ): Contracts.TimerActivityRes {
        spot.activity(request.value)
        return spot.activityStatus()
    }
}
