package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.entryspot.handlers

import kotlinx.coroutines.future.await
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.server.support.application.AgentAssignmentService
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportActorDirectory
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActor
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.entryspot.SupportEntrySpot
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SetAgentAvailableReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SetAgentAvailableRes

class SetAgentAvailableHandler(
    private val assignment: AgentAssignmentService,
    private val actors: ZLinkActorManager,
    private val directory: SupportActorDirectory,
) : ZLinkSuspendingEntrySpotActorRequestHandler<
    SupportEntrySpot,
    SupportUserActor,
    SetAgentAvailableReq,
    SetAgentAvailableRes,
    > {
    override suspend fun handle(
        entrySpot: SupportEntrySpot,
        actor: SupportUserActor,
        context: ZLinkMessageContext,
        request: SetAgentAvailableReq,
    ): SetAgentAvailableRes {
        if (actor.role != SupportChatRoles.Agent) {
            throw IllegalStateException("Only agent actors can set availability.")
        }
        val actorRef = actors.find(actor.actorId).await().orElse(null)
            ?: throw IllegalStateException("Support actor ref is not available. actor=${actor.actorId}")
        directory.addOrUpdate(actor, actorRef)
        assignment.setAvailable(actor.actorId, actor.displayName, request.isAvailable)
        return SetAgentAvailableRes(request.isAvailable)
    }
}
