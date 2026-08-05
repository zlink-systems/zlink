package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import systems.zlink.framework.handlers.ZLinkSpotRequest

class SlowRequestHandler {
    @ZLinkSpotRequest
    suspend fun handle(
        spot: UserSpot,
        request: Contracts.SlowReq,
    ): Contracts.StateRes {
        kotlinx.coroutines.delay(1000)
        return Contracts.StateRes(
            spot.context().spotId(),
            spot.context().nodeRid().toString(),
            spot.apply("slow:${request.value}"),
        )
    }
}
