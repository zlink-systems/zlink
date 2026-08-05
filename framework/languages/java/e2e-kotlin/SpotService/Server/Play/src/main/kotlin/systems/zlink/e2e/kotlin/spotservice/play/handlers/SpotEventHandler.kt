package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import systems.zlink.framework.handlers.ZLinkSpotSubscription
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotSubscriptionHandler

@ZLinkSpotSubscription(topic = "spot.events")
class SpotEventHandler : ZLinkSuspendingSpotSubscriptionHandler<UserSpot, Contracts.MeshMsg> {
    override suspend fun handle(
        spot: UserSpot,
        message: Contracts.MeshMsg
    ) {
        spot.record("SpotMeshEvent", message.value)
    }
}
