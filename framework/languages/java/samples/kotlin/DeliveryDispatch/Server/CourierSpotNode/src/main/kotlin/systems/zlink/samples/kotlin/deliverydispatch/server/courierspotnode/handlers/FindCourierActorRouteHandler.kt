package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.handlers

import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotRequestHandler
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot
import systems.zlink.framework.actors.ActorRefSnapshot
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.FindCourierActorReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.FindCourierActorRes

class FindCourierActorRouteHandler(
    private val actors: ZLinkActorManager,
) : ZLinkSuspendingSpotRequestHandler<CourierEntrySpot, FindCourierActorReq, FindCourierActorRes> {
    override suspend fun handle(
        spot: CourierEntrySpot,
        request: FindCourierActorReq,
    ): FindCourierActorRes {
        val actor = actors.find(request.courierId).await()
        return FindCourierActorRes(
            courierId = request.courierId,
            actor = actor.map { ActorRefSnapshot.from(it) }.orElse(null),
        )
    }
}
