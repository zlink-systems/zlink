package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.play.spots.ScenarioStage
import systems.zlink.e2e.kotlin.spotservice.play.spots.UserSpot
import systems.zlink.framework.handlers.ZLinkSpotRequest
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotPacketHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotTimerHandler
import systems.zlink.framework.spots.ZLinkTimerTick

class StageProbeHandler {
    @ZLinkSpotRequest
    suspend fun handle(
        spot: UserSpot,
        request: Contracts.StageProbeReq,
    ): Contracts.StateRes =
        ScenarioStage(spot).apply(request)
}

class StageTimerStartHandler : ZLinkSuspendingSpotPacketHandler<UserSpot, Contracts.StageTimerStartMsg> {
    override suspend fun handle(spot: UserSpot, message: Contracts.StageTimerStartMsg) {
        ScenarioStage(spot).startTimer(message)
    }
}

class StageTimerHandler : ZLinkSuspendingSpotTimerHandler<UserSpot> {
    override suspend fun handle(spot: UserSpot, tick: ZLinkTimerTick) {
        spot.record("StageTimer", "${tick.name()}#${tick.deliveryIndex()}")
    }
}
