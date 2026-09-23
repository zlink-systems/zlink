package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.entryspot.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActor
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.entryspot.SupportEntrySpot
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationRes

class JoinConversationActorHandler :
    ZLinkSuspendingEntrySpotActorRequestHandler<
        SupportEntrySpot,
        SupportUserActor,
        JoinConversationReq,
        JoinConversationRes,
    > {
    override suspend fun handle(
        entrySpot: SupportEntrySpot,
        actor: SupportUserActor,
        context: ZLinkMessageContext,
        request: JoinConversationReq,
    ): JoinConversationRes {
        if (actor.role != SupportChatRoles.Agent) {
            throw IllegalStateException(
                "Only agent conversation actors can join through the Entry Spot"
            )
        }
        val conversationId =
            request.conversationId.takeIf(String::isNotBlank)
                ?: throw IllegalStateException("Conversation Join requires a conversation ID")
        return actor.scheduleConversationJoin(
            conversationId,
            "",
            JoinConversationReq(conversationId, actor.participantId, actor.role, actor.displayName),
        )
    }
}
