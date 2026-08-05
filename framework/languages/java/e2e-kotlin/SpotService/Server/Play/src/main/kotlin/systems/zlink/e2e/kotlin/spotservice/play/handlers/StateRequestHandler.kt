package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import systems.zlink.framework.handlers.ZLinkSpotRequest

class StateRequestHandler {
    @ZLinkSpotRequest
    suspend fun handle(
        spot: UserSpot,
        request: Contracts.StateReq,
    ): Contracts.StateRes {
        val value = if (request.op == "worker-start" || request.op == "worker-start-long") {
            spot.startWorker(request.op)
        } else {
            spot.apply(request.op)
        }
        return Contracts.StateRes(
            spot.context().spotId(),
            spot.context().nodeRid().toString(),
            value,
        )
    }
}
