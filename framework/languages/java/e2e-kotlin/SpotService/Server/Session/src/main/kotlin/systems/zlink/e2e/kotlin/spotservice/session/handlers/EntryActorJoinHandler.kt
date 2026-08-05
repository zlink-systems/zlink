package systems.zlink.e2e.kotlin.spotservice.session.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.session.spots.ScenarioActor
import systems.zlink.e2e.kotlin.spotservice.session.spots.ScenarioEntrySpot
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.handlers.ZLinkSpotActorRequest
import systems.zlink.framework.ZLinkMessageContext

class EntryActorJoinHandler : ZLinkSuspendingEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, Contracts.ActorJoinReq, Contracts.ActorJoinRes> {
    @ZLinkSpotActorRequest(packetName = "ActorJoinReq")
    override suspend fun handle(
        spot: ScenarioEntrySpot,
        actor: ScenarioActor,
        context: ZLinkMessageContext,
        request: Contracts.ActorJoinReq,
    ): Contracts.ActorJoinRes {
        actor.applyProfile(request.profile)
        spot.record("ActorJoinPayload", payloadEvidence(request))
        actor.context()
            .joinSpot(request.spotRid, request)
            .defer()
        return Contracts.ActorJoinRes(
            actor.actorId(),
            request.spotRid,
            spot.nodeRid().toString(),
            request.profile.displayName,
            request.profile.level,
            request.tags,
        )
    }

    private fun payloadEvidence(request: Contracts.ActorJoinReq): String {
        return request.profile.displayName +
            "/" + request.profile.level +
            "/" + request.tags.joinToString(",")
    }
}
