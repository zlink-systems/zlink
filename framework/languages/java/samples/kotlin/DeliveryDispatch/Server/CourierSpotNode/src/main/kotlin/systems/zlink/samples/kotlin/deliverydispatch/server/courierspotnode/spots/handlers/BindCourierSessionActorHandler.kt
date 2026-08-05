package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots.handlers

import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.CourierActor
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.BindCourierSessionReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.BindCourierSessionRes

class BindCourierSessionActorHandler : ZLinkSuspendingEntrySpotActorRequestHandler<
    CourierEntrySpot,
    CourierActor,
    BindCourierSessionReq,
    BindCourierSessionRes,
    > {
    override suspend fun handle(
        entrySpot: CourierEntrySpot,
        actor: CourierActor,
        context: ZLinkMessageContext,
        request: BindCourierSessionReq,
    ): BindCourierSessionRes =
        BindCourierSessionRes(
            courierId = request.courierId,
            actor = requireNotNull(request.actor) { "BindCourierSessionReq.actor is required" },
            sessionRoute = requireNotNull(request.sessionRoute) { "BindCourierSessionReq.sessionRoute is required" },
        )
}
