package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.spots.handlers

import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.CustomerActor
import systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.spots.CustomerEntrySpot
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.SubscribeDeliveryReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.SubscribeDeliveryRes

class SubscribeDeliveryActorHandler : ZLinkSuspendingEntrySpotActorRequestHandler<
    CustomerEntrySpot,
    CustomerActor,
    SubscribeDeliveryReq,
    SubscribeDeliveryRes,
    > {
    override suspend fun handle(
        entrySpot: CustomerEntrySpot,
        actor: CustomerActor,
        context: ZLinkMessageContext,
        request: SubscribeDeliveryReq,
    ): SubscribeDeliveryRes =
        entrySpot.subscribe(actor, request)
}
