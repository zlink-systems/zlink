package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots

import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpot
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkActorCreateResponse
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.ActorDirectory
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.CourierActor

class CourierEntrySpot(
    override val context: ZLinkEntrySpotContext,
    private val actors: ActorDirectory,
) : ZLinkSuspendingEntrySpot<CourierActor>() {
    override suspend fun onCreateActorSuspending(
        actor: CourierActor,
        createRequest: ZLinkMessage,
    ): ZLinkActorCreateResponse {
        actors.register(actor)
        return ZLinkActorCreateResponse.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: CourierActor) {
        actors.register(actor)
    }

    override suspend fun onLeaveActorSuspending(actor: CourierActor) {
        actors.remove(actor.actorId())
    }
}
