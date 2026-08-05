package systems.zlink.e2e.kotlin.runtimemonitoring.service

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotTimerHandler
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkTimerOptions
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy
import systems.zlink.framework.spots.ZLinkTimerTick
import java.time.Duration

class MonitoringSpot(
    override val context: ZLinkSpotContext,
) : ZLinkSuspendingSpot<ZLinkActor>() {
    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse {
        val options = ZLinkTimerOptions(
            ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS,
            1,
            false,
        )
        context.addTimer(
            "failing-monitoring-timer",
            Duration.ofMillis(500),
            FailingTimerHandler::class.java,
            options,
        )
        context.addTimer(
            "stopping-monitoring-timer",
            Duration.ofMillis(500),
            FailingTimerHandler::class.java,
            null,
        )
        return ZLinkSpotCreateResponse.accept()
    }

    override suspend fun onActorJoinSuspending(actorId: String, request: ZLinkMessage) =
        ZLinkSpotActorJoinResult.reject("actors are not supported")

    override suspend fun onJoinedActorSuspending(actor: ZLinkActor) = Unit

    override suspend fun onLeaveActorSuspending(actor: ZLinkActor) = Unit

    class FailingTimerHandler : ZLinkSuspendingSpotTimerHandler<MonitoringSpot> {
        override suspend fun handle(spot: MonitoringSpot, tick: ZLinkTimerTick) {
            throw IllegalStateException("monitoring timer boom")
        }
    }
}
