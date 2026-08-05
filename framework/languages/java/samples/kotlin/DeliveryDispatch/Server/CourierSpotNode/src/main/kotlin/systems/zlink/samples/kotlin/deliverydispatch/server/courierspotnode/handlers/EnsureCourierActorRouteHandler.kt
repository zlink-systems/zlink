package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.handlers

import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotRequestHandler
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot
import systems.zlink.framework.actors.ActorRefSnapshot
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.EnsureCourierActorRes
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.EnsureCourierActorReq

class EnsureCourierActorRouteHandler(
    private val actors: ZLinkActorManager,
) : ZLinkSuspendingSpotRequestHandler<CourierEntrySpot, EnsureCourierActorReq, EnsureCourierActorRes> {
    override suspend fun handle(
        spot: CourierEntrySpot,
        request: EnsureCourierActorReq,
    ): EnsureCourierActorRes {
        val actor = actors.kotlin()
            .getOrCreate(request.courierId, SampleNames.CourierActorType)
            .request(request)
            .await()
        return EnsureCourierActorRes(
            courierId = request.courierId,
            actor = ActorRefSnapshot.from(requireActor(actor)),
        )
    }

    private fun requireActor(result: ZLinkActorCreateResult): ActorRef = when (result) {
        is ZLinkActorCreateResult.Existing -> result.actor
        is ZLinkActorCreateResult.Created -> result.actor
        is ZLinkActorCreateResult.Rejected -> throw IllegalStateException("Courier actor creation was rejected.")
    }
}
