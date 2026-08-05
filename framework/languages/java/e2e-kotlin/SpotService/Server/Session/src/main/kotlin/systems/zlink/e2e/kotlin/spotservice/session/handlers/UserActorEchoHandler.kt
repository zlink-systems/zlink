package systems.zlink.e2e.kotlin.spotservice.session.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.session.spots.ScenarioActor
import systems.zlink.e2e.kotlin.spotservice.session.spots.UserSpot
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorRequestHandler
import systems.zlink.framework.handlers.ZLinkSpotActorRequest
import systems.zlink.framework.ZLinkMessageContext

class UserActorEchoHandler : ZLinkSuspendingSpotActorRequestHandler<UserSpot, ScenarioActor, Contracts.ActorEchoReq, Contracts.ActorEchoRes> {
    @ZLinkSpotActorRequest(packetName = "ActorEchoReq")
    override suspend fun handle(
        spot: UserSpot,
        actor: ScenarioActor,
        context: ZLinkMessageContext,
        request: Contracts.ActorEchoReq,
    ): Contracts.ActorEchoRes {
        val seq = actor.nextSequence()
        spot.record("ActorUserRequest", actor.actorId() + "/" + request.value + "#" + seq)
        actor.context().boundSession()
            .send(Contracts.ActorPushNotify(actor.actorId(), spot.spotRid(), "push:" + request.value, request.seq, seq))
            .submit()
        return Contracts.ActorEchoRes(
            actor.actorId(),
            spot.spotRid(),
            spot.nodeRid(),
            "user:" + request.value,
            request.seq,
            seq,
            request.profile.displayName,
            request.profile.level,
            request.profile.tags
        )
    }
}
