package systems.zlink.e2e.kotlin.spotservice.session.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.session.spots.ScenarioActor
import systems.zlink.e2e.kotlin.spotservice.session.spots.UserSpot
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorRequestHandler
import systems.zlink.framework.handlers.ZLinkSpotActorRequest
import systems.zlink.framework.ZLinkMessageContext

class UserActorLeaveHandler : ZLinkSuspendingSpotActorRequestHandler<UserSpot, ScenarioActor, Contracts.LeaveActorReq, Contracts.LeaveActorRes> {
    @ZLinkSpotActorRequest(packetName = "LeaveActorReq")
    override suspend fun handle(
        spot: UserSpot,
        actor: ScenarioActor,
        context: ZLinkMessageContext,
        request: Contracts.LeaveActorReq,
    ): Contracts.LeaveActorRes {
        if (request.actorId != actor.actorId()) {
            throw IllegalStateException("leave request actor does not match dispatched actor")
        }
        spot.context().leaveActor(actor).await()
        return Contracts.LeaveActorRes(actor.actorId(), true)
    }
}
