package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.spots

import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpot
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkActorCreateResponse
import systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.CustomerActor
import systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.CustomerActorDirectory
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.SubscribeDeliveryReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.SubscribeDeliveryRes

class CustomerEntrySpot(
    override val context: ZLinkEntrySpotContext,
    private val customers: CustomerActorDirectory,
) : ZLinkSuspendingEntrySpot<CustomerActor>() {
    override suspend fun onCreateActorSuspending(
        actor: CustomerActor,
        createRequest: ZLinkMessage,
    ): ZLinkActorCreateResponse {
        customers.register(actor)
        return ZLinkActorCreateResponse.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: CustomerActor) {
        customers.register(actor)
    }

    override suspend fun onLeaveActorSuspending(actor: CustomerActor) {
        customers.remove(actor.actorId())
    }

    fun subscribe(
        actor: CustomerActor,
        request: SubscribeDeliveryReq,
    ): SubscribeDeliveryRes {
        customers.subscribe(actor.actorId(), request.deliveryId)
        return SubscribeDeliveryRes(request.deliveryId)
    }
}
