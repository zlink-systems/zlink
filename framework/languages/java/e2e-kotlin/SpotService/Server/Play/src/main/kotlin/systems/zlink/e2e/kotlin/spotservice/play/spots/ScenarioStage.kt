package systems.zlink.e2e.kotlin.spotservice.play.spots

import java.time.Duration
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.play.handlers.StageTimerHandler

internal class ScenarioStage(
    private val spot: UserSpot,
) {
    fun apply(request: Contracts.StageProbeReq): Contracts.StateRes {
        val value = spot.apply(request.op)
        spot.record("StageRequest", "${request.marker}|$value")
        return Contracts.StateRes(
            spot.context().spotId(),
            spot.context().nodeRid().toString(),
            value,
        )
    }

    fun startTimer(command: Contracts.StageTimerStartMsg) {
        spot.context().addTimer(
            command.name,
            Duration.ofMillis(command.periodMilliseconds.coerceAtLeast(1).toLong()),
            StageTimerHandler::class.java,
            null
        )
    }
}
