package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.handlers

import org.slf4j.LoggerFactory
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.samples.kotlin.supportchat.server.configuration.ConversationStatuses
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.support.application.SupportConversationAllocator
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AllocateConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AllocateConversationRes

@ZLinkHandlerGroup(SampleNames.SupportChannel)
class AllocateConversationHandler(
    private val allocator: SupportConversationAllocator,
) : ZLinkSuspendingRequestHandler<AllocateConversationReq, AllocateConversationRes> {
    override suspend fun handle(
        request: AllocateConversationReq,
        context: ZLinkMessageContext,
    ): AllocateConversationRes {
        logger.info(
            "support allocate: request customer={} subject={}",
            request.customerActorId,
            request.subject,
        )
        val conversationId = allocator.allocate(
            request.customerActorId,
            request.customerDisplayName,
            request.subject,
        )
        logger.info("support allocate: created conversation={}", conversationId)
        return AllocateConversationRes(conversationId, ConversationStatuses.WaitingForAgent)
    }

    private companion object {
        private val logger = LoggerFactory.getLogger(AllocateConversationHandler::class.java)
    }
}
