package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import kotlinx.coroutines.future.await
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
            spot.nodeRid(),
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
