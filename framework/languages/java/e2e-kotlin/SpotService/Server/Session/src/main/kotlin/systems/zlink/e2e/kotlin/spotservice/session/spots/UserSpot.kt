package systems.zlink.e2e.kotlin.spotservice.session.spots

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.session.handlers.UserActorEchoHandler
import systems.zlink.e2e.kotlin.spotservice.session.handlers.UserActorLeaveHandler
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

    override fun configure() {
        context.handlers().addHandler<UserActorEchoHandler>()
        context.handlers().addHandler<UserActorLeaveHandler>()
    }

    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse {
        evidence.record("SpotCreated", context.spotId(), if (request.isEmpty) "" else "request")
        return ZLinkSpotCreateResponse.accept()
    }

    override suspend fun onInitializeSuspending() {
        evidence.record("SpotInitialized", context.spotId(), "")
    }

    override suspend fun onClosingSuspending(
        closingContext: systems.zlink.framework.spots.ZLinkSpotClosingContext,
    ) {
        evidence.record("SpotClosing", context.spotId(), "")
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

    fun record(marker: String, value: String) {
        evidence.record(marker, context.spotId(), value)
    }

    fun spotRid(): String = context.spotId()

    fun nodeRid(): String = evidence.nodeRid()
}
