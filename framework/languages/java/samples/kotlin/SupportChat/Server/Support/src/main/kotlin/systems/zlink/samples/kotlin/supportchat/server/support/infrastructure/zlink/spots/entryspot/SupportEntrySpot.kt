package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.entryspot

import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpot
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkActorCreateResponse
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.server.support.application.AgentAssignmentService
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportActorDirectory
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActor
import systems.zlink.samples.kotlin.supportchat.shared.contracts.EnsureSupportUserActorReq

class SupportEntrySpot(
    override val context: ZLinkEntrySpotContext,
    private val directory: SupportActorDirectory,
    private val assignment: AgentAssignmentService,
    private val actorManager: ZLinkActorManager,
) : ZLinkSuspendingEntrySpot<SupportUserActor>() {
    override suspend fun onCreateActorSuspending(
        actor: SupportUserActor,
        createRequest: ZLinkMessage,
    ): ZLinkActorCreateResponse {
        val request = createRequest.decode(EnsureSupportUserActorReq::class.java)
        actor.setIdentity(request.displayName, request.role, request.participantId)
        val actorRef = actorManager.find(actor.actorId).await().orElse(null)
            ?: throw IllegalStateException("Support actor ref is not available. actor=${actor.actorId}")
        directory.addOrUpdate(actor, actorRef)
        return ZLinkActorCreateResponse.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: SupportUserActor) {
    }

    override suspend fun onLeaveActorSuspending(actor: SupportUserActor) {
    }

    override suspend fun onDisconnectActorSuspending(actor: SupportUserActor) {
        if (actor.role == SupportChatRoles.Agent) {
            assignment.setAvailable(actor.actorId, actor.displayName, false)
        }
    }
}
