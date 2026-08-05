package systems.zlink.e2e.kotlin.spotservice.play.spots

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.handlers.*
import java.time.Duration
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse

class UserSpot(
    override val context: ZLinkSpotContext,
    private val evidence: ScenarioState
) : ZLinkSuspendingSpot<ScenarioActor>() {
    private val pendingProfiles = mutableMapOf<String, Contracts.ActorProfile>()
    private var state = ""
    private var workerDone = true

    override fun configure() {
        context.handlers().addHandler<StateRequestHandler>()
        context.handlers().addHandler<StateCommandHandler>()
        context.handlers().addHandler<StageProbeHandler>()
        context.handlers().addHandler<StageTimerStartHandler>()
        context.handlers().addHandler<SlowRequestHandler>()
        context.handlers().addHandler<OutboundRequestHandler>()
        context.handlers().addHandler<SpotToSpotCommandHandler>()
        context.handlers().addHandler<OutboundCommandHandler>()
        context.handlers().addHandler<SpotEventHandler>()
        context.handlers().addHandler<UserActorEchoHandler>()
        context.handlers().addHandler<UserActorLeaveHandler>()
    }

    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse {
        evidence.record("SpotCreated", context.spotId(), if (request.isEmpty) "" else "request")
        context.addTimer("state-timer", Duration.ofSeconds(2), StateTimerHandler::class.java, null)
        return ZLinkSpotCreateResponse.accept()
    }

    override suspend fun onInitializeSuspending() {
        evidence.record("SpotInitialized", context.spotId(), "")
    }

    override suspend fun onClosingSuspending(
        closingContext: systems.zlink.framework.spots.ZLinkSpotClosingContext,
    ) {
        evidence.record("SpotClosing", context.spotId(), state)
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val join = request.decode(Contracts.ActorJoinReq::class.java)
        pendingProfiles[actorId] = join.profile
        evidence.record(
            "ActorUserJoinRequested",
            context.spotId(),
            actorId + "/" + join.profile.displayName + "/" + join.tags.joinToString(",")
        )
        return ZLinkSpotActorJoinResult.accept(
            Contracts.ActorJoinRes(
                actorId,
                context.spotId(),
                evidence.nodeRid(),
                join.profile.displayName,
                join.profile.level,
                join.tags
            )
        )
    }

    override suspend fun onJoinedActorSuspending(actor: ScenarioActor) {
        actor.applyProfile(
            pendingProfiles.remove(actor.actorId())
                ?: error("joined actor does not have a pending admission")
        )
        evidence.record("ActorUserJoined", context.spotId(), actor.actorId() + "#" + actor.nextSequence())
    }

    override suspend fun onLeaveActorSuspending(actor: ScenarioActor) {
        evidence.record("ActorUserLeft", context.spotId(), actor.actorId())
    }

    override suspend fun onDisconnectActorSuspending(actor: ScenarioActor) {
        evidence.record("ActorUserDisconnected", context.spotId(), actor.actorId())
    }

    fun apply(op: String): String {
        state = if (state.isBlank()) op else "$state,$op"
        evidence.record("StateReq", context.spotId(), state)
        if (op == "worker-follow-up" && !workerDone) {
            evidence.record("WorkerFollowUpBeforeComplete", context.spotId(), state)
        }
        return state
    }

    fun startWorker(op: String): String {
        workerDone = false
        evidence.record("WorkerStarted", context.spotId(), op)
        context.runCpuWorker {
            val delayMillis = if (op == "worker-start-long") 5000L else 1500L
            Thread.sleep(delayMillis)
            "$op-done"
        }.submit().whenComplete { value, error ->
            if (error != null) {
                evidence.record("WorkerFailed", context.spotId(), error.javaClass.simpleName)
                return@whenComplete
            }
            workerDone = true
            state = if (state.isBlank()) value else "$state,$value"
            evidence.record("WorkerCompleted", context.spotId(), value)
        }
        return state
    }

    fun command(value: String) {
        evidence.record("StateMsg", context.spotId(), value)
    }

    fun record(marker: String, value: String) {
        evidence.record(marker, context.spotId(), value)
    }

    fun spotRid(): String = context.spotId()

    fun nodeRid(): String = evidence.nodeRid()

    fun timerTick(deliveryIndex: Long) {
        evidence.record("SpotTimer", context.spotId(), deliveryIndex.toString())
    }
}
