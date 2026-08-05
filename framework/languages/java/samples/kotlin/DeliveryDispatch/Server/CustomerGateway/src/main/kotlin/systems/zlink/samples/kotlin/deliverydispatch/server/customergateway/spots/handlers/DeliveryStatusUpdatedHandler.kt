package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.spots.handlers

import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorSendHandler
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.CustomerActor
import systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.spots.CustomerEntrySpot
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatusNotify
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatusUpdatedMsg

class DeliveryStatusUpdatedHandler : ZLinkSuspendingEntrySpotActorSendHandler<
    CustomerEntrySpot,
    CustomerActor,
    DeliveryStatusUpdatedMsg,
    > {
    override suspend fun handle(
        entrySpot: CustomerEntrySpot,
        actor: CustomerActor,
        context: ZLinkMessageContext,
        message: DeliveryStatusUpdatedMsg,
    ) {
        actor.push(
            DeliveryStatusNotify(
                deliveryId = message.deliveryId,
                status = message.status,
                courierId = message.courierId,
                occurredAt = message.occurredAt,
            ),
        )
    }
}
