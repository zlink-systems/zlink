package systems.zlink.e2e.kotlin.spotservice.play.spots


import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.handlers.*
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.spots.ZLinkSpotContext

class MismatchedSpot(
    override val context: ZLinkSpotContext
) : ZLinkSuspendingSpot<ZLinkActor>() {
    override suspend fun onActorJoinSuspending(actorId: String, request: ZLinkMessage) = ZLinkSpotActorJoinResult.reject("unsupported")
    override suspend fun onJoinedActorSuspending(actor: ZLinkActor) {
    }

    override suspend fun onLeaveActorSuspending(actor: ZLinkActor) {
    }
    override fun configure() {
    }
}
