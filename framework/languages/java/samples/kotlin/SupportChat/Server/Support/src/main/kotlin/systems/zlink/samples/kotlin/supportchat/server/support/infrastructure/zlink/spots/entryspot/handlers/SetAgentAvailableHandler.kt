package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.entryspot.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.server.support.application.AgentAssignmentService
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportActorDirectory
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActor
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.entryspot.SupportEntrySpot
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SetAgentAvailableReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SetAgentAvailableRes

class SetAgentAvailableHandler(
    private val assignment: AgentAssignmentService,
    private val directory: SupportActorDirectory,
) :
    ZLinkSuspendingEntrySpotActorRequestHandler<
        SupportEntrySpot,
        SupportUserActor,
        SetAgentAvailableReq,
        SetAgentAvailableRes,
    > {
    // --8<-- [start:doc-sc-set-available]
    override suspend fun handle(
        entrySpot: SupportEntrySpot,
        actor: SupportUserActor,
        context: ZLinkMessageContext,
        request: SetAgentAvailableReq,
    ): SetAgentAvailableRes {
        if (actor.role != SupportChatRoles.Agent) {
            throw IllegalStateException("Only agent actors can set availability.")
        }
        directory.addOrUpdate(actor)
        assignment.setAvailable(actor.actorId, actor.displayName, request.isAvailable)
        return SetAgentAvailableRes(request.isAvailable)
    }
    // --8<-- [end:doc-sc-set-available]
}
