package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.handlers

import org.slf4j.LoggerFactory
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ActorRefSnapshot
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.shared.contracts.EnsureAgentConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.EnsureAgentConversationRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.EnsureSupportUserActorReq

@ZLinkHandlerGroup(SampleNames.SupportChannel)
class EnsureAgentConversationHandler(private val actors: ZLinkActorManager) :
    ZLinkSuspendingRequestHandler<EnsureAgentConversationReq, EnsureAgentConversationRes> {

    override suspend fun handle(
        request: EnsureAgentConversationReq,
        context: ZLinkMessageContext,
    ): EnsureAgentConversationRes {
        val conversationActorId = "${request.rosterActorId}@${request.conversationId}"
        val existingActorRef = actors.find(conversationActorId).await().orElse(null)
        val zlinkActorRef =
            existingActorRef
                ?: actors
                    .kotlin()
                    .getOrCreate(conversationActorId, SampleNames.SupportActorType)
                    .request(
                        EnsureSupportUserActorReq(
                            actorId = conversationActorId,
                            displayName = request.displayName,
                            role = SupportChatRoles.Agent,
                            participantId = request.rosterActorId,
                        )
                    )
                    .await()
                    .requireActor()
        val actorRef: ActorRef = zlinkActorRef

        logger.info(
            "support agent conversation: ensured. conversation={}, roster={}",
            request.conversationId,
            request.rosterActorId,
        )
        return EnsureAgentConversationRes(ActorRefSnapshot.from(actorRef))
    }

    private fun ZLinkActorCreateResult.requireActor(): ActorRef =
        when (this) {
            is ZLinkActorCreateResult.Created -> actor
            is ZLinkActorCreateResult.Existing -> actor
            is ZLinkActorCreateResult.Rejected ->
                error("Agent conversation actor creation was rejected")
        }

    private companion object {
        private val logger = LoggerFactory.getLogger(EnsureAgentConversationHandler::class.java)
    }
}
