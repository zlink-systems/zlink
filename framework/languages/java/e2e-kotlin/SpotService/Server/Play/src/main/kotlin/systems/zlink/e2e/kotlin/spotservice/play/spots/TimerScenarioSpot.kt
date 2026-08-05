package systems.zlink.e2e.kotlin.spotservice.play.spots

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.handlers.*
import java.time.Duration
import java.time.Instant
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.framework.spots.ZLinkTimer
import systems.zlink.framework.spots.ZLinkTimerOptions
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy

class TimerScenarioSpot(
    override val context: ZLinkSpotContext,
    private val evidence: ScenarioState
) : ZLinkSuspendingSpot<ZLinkActor>() {
    override suspend fun onActorJoinSuspending(actorId: String, request: ZLinkMessage) = systems.zlink.framework.spots.ZLinkSpotActorJoinResult.reject("unsupported")
    override suspend fun onJoinedActorSuspending(actor: ZLinkActor) {
    }

    override suspend fun onLeaveActorSuspending(actor: ZLinkActor) {
    }
    private var lastActivity = Instant.now()
    private var tickCount = 0
    private var skippedTicks = 0L
    private var status = "open"
    private var overrunTimer: ZLinkTimer? = null
    private var idleKeepRecorded = false

    override fun configure() {
        context.handlers().addHandler<TimerActivityHandler>()
        context.handlers().addHandler<TimerStatusHandler>()
    }

    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse {
        val rid = context.spotId()
        if (rid.startsWith("timer-overrun-")) {
            val overrunPolicy = when {
                rid.endsWith("catchup") -> ZLinkTimerOverrunPolicy.CATCH_UP_BOUNDED
                rid.endsWith("delay") -> ZLinkTimerOverrunPolicy.DELAY_NEXT_TICK
                else -> ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS
            }
            val options = ZLinkTimerOptions(
                overrunPolicy,
                if (rid.endsWith("catchup")) 2 else 1,
                false,
            )
            context.addTimer("overrun", Duration.ofMillis(50), TimerOverrunHandler::class.java, options)
                .thenAccept { timer -> overrunTimer = timer }
            evidence.record("TimerOverrunConfigured", rid, options.overrunPolicy().name)
        } else {
            context.addTimer("idle", Duration.ofMillis(250), IdleCloseTimerHandler::class.java, null)
            evidence.record("IdleTimerConfigured", rid, "idle")
        }
        return ZLinkSpotCreateResponse.accept()
    }

    override suspend fun onClosingSuspending(
        closingContext: systems.zlink.framework.spots.ZLinkSpotClosingContext,
    ) {
        status = "closed"
        evidence.record("IdleClosed", context.spotId(), "closed")
    }

    fun activity(value: String) {
        lastActivity = Instant.now()
        status = "active:$value"
        evidence.record("IdleActivity", context.spotId(), value)
    }

    fun activityStatus(): Contracts.TimerActivityRes =
        Contracts.TimerActivityRes(context.spotId(), status)

    fun status(): Contracts.TimerStatusRes = Contracts.TimerStatusRes(context.spotId(), status)

    fun idleTick() {
        val rid = context.spotId()
        if (rid == "idle-close" &&
            Duration.between(lastActivity, Instant.now()).compareTo(Duration.ofMillis(700)) > 0
        ) {
            evidence.record("IdleCloseRequested", rid, "idle")
            context.close()
            return
        }
        if (rid == "idle-active" && !idleKeepRecorded) {
            idleKeepRecorded = true
            evidence.record("IdleKeptOpen", rid, status)
        }
    }

    fun overrunTick(deliveryIndex: Long, skipped: Long) {
        tickCount += 1
        skippedTicks += skipped
        val rid = context.spotId()
        status = "ticks=$tickCount,skipped=$skippedTicks"
        evidence.record("TimerOverrunTick", rid, "$deliveryIndex/$skipped/$tickCount")
        if (tickCount >= 3 && overrunTimer != null) {
            overrunTimer!!.close()
            evidence.record("TimerOverrunStopped", rid, status)
        }
    }
}
