package systems.zlink.e2e.kotlin.spotservice.session.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.session.spots.ScenarioActor
import systems.zlink.e2e.kotlin.spotservice.session.spots.ScenarioEntrySpot
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.handlers.ZLinkSpotActorRequest
import systems.zlink.framework.ZLinkMessageContext

class EntryActorDestroyHandler : ZLinkSuspendingEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, Contracts.DestroyActorReq, Contracts.DestroyActorRes> {
    @ZLinkSpotActorRequest(packetName = "DestroyActorReq")
    override suspend fun handle(
        spot: ScenarioEntrySpot,
        actor: ScenarioActor,
        context: ZLinkMessageContext,
        request: Contracts.DestroyActorReq,
    ): Contracts.DestroyActorRes {
        if (request.actorId != actor.actorId()) {
            throw IllegalStateException("destroy request actor does not match dispatched actor")
        }

        try {
            spot.context().runCpuWorker { true }.submit().await()
            spot.context().destroyActor(actor).await()
            spot.record("ActorDestroyed", actor.actorId())
        } catch (error: Throwable) {
            spot.record("ActorDestroyFailed", actor.actorId() + "/" + error.javaClass.simpleName)
        }

        return Contracts.DestroyActorRes(actor.actorId(), true)
    }
}
