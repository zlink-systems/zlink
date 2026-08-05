package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.handlers

import kotlinx.coroutines.future.await
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ActorRefSnapshot
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.shared.contracts.EnsureSupportUserActorReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.EnsureSupportUserActorRes

@ZLinkHandlerGroup(SampleNames.SupportChannel)
class EnsureSupportUserActorHandler(
    private val actors: ZLinkActorManager,
) : ZLinkSuspendingRequestHandler<EnsureSupportUserActorReq, EnsureSupportUserActorRes> {
    override suspend fun handle(
        request: EnsureSupportUserActorReq,
        context: ZLinkMessageContext,
    ): EnsureSupportUserActorRes {
        val existing = actors.find(request.actorId).await().orElse(null)
        if (existing != null) {
            return existing.toResponse()
        }
        val actor = actors
            .kotlin()
            .getOrCreate(request.actorId, SampleNames.SupportActorType)
            .request(request)
            .await()
        return actor.requireActor().toResponse()
    }
}

private fun ActorRef.toResponse(): EnsureSupportUserActorRes =
    EnsureSupportUserActorRes(
        actor = ActorRefSnapshot.from(this),
    )

private fun ZLinkActorCreateResult.requireActor(): ActorRef = when (this) {
    is ZLinkActorCreateResult.Created -> actor
    is ZLinkActorCreateResult.Existing -> actor
    is ZLinkActorCreateResult.Rejected -> error("Support actor creation was rejected")
}
