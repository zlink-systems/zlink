package systems.zlink.e2e.kotlin.spotservice.play.spots

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.handlers.*
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.spots.ZLinkSpotContext

class MismatchedSpot(
    override val context: ZLinkSpotContext
) : ZLinkSuspendingSpot<ZLinkActor>() {
    override suspend fun onActorJoinSuspending(actorId: String, request: systems.zlink.framework.messaging.ZLinkMessage) = systems.zlink.framework.spots.ZLinkSpotActorJoinResult.reject("unsupported")
    override suspend fun onJoinedActorSuspending(actor: ZLinkActor) {
    }

    override suspend fun onLeaveActorSuspending(actor: ZLinkActor) {
    }
    override fun configure() {
    }
}
