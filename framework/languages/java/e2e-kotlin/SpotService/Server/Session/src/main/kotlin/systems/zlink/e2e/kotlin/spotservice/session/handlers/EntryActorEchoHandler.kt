package systems.zlink.e2e.kotlin.spotservice.session.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.session.spots.ScenarioActor
import systems.zlink.e2e.kotlin.spotservice.session.spots.ScenarioEntrySpot
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.handlers.ZLinkSpotActorRequest
import systems.zlink.framework.ZLinkMessageContext

class EntryActorEchoHandler : ZLinkSuspendingEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, Contracts.ActorEchoReq, Contracts.ActorEchoRes> {
    @ZLinkSpotActorRequest(packetName = "ActorEchoReq")
    override suspend fun handle(
        spot: ScenarioEntrySpot,
        actor: ScenarioActor,
        context: ZLinkMessageContext,
        request: Contracts.ActorEchoReq,
    ): Contracts.ActorEchoRes {
        val seq = actor.nextSequence()
        spot.record("ActorEntryRequest", actor.actorId() + "/" + request.value + "#" + seq)
        actor.context().boundSession()
            .send(Contracts.ActorPushNotify(actor.actorId(), "entry", "push:" + request.value, request.seq, seq))
            .submit()
        return Contracts.ActorEchoRes(
            actor.actorId(),
            "entry",
            spot.nodeRid(),
            "entry:" + request.value,
            request.seq,
            seq,
            request.profile.displayName,
            request.profile.level,
            request.profile.tags
        )
    }
}
