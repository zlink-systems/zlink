package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.channels.ZLinkRouteMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRouteRequestHandler
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin

class EnsureActorHandler(
    private val actors: ZLinkActorManager,
    private val state: ScenarioState,
) : ZLinkSuspendingRouteRequestHandler<Contracts.EnsureActorReq, Contracts.EnsureActorRes> {
    override suspend fun handle(
        request: Contracts.EnsureActorReq,
        context: ZLinkRouteMessageContext,
    ): Contracts.EnsureActorRes {
        val actor = when (val result = actors.kotlin()
            .getOrCreate(request.actorId, "scenario")
            .await()
        ) {
            is ZLinkActorCreateResult.Existing -> result.actor
            is ZLinkActorCreateResult.Created -> result.actor
            is ZLinkActorCreateResult.Rejected ->
                error("actor creation was rejected")
        }
        state.record("EnsureActor", "entry", request.actorId)
        return Contracts.EnsureActorRes(
            actor.actorId(),
            actor.nodeRid().toString(),
            actor.objectGeneration()
        )
    }
}
