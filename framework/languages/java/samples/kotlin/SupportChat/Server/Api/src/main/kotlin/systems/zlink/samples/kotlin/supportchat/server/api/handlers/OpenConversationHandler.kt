package systems.zlink.samples.kotlin.supportchat.server.api.handlers

import kotlinx.coroutines.future.await
import org.slf4j.LoggerFactory
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AllocateConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AllocateConversationRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.OpenConversationApiReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.OpenConversationApiRes

@ZLinkHandlerGroup(SampleNames.ApiChannel)
class OpenConversationHandler(
    private val channels: ZLinkClient,
) : ZLinkSuspendingRequestHandler<OpenConversationApiReq, OpenConversationApiRes> {
    override suspend fun handle(
        request: OpenConversationApiReq,
        context: ZLinkMessageContext,
    ): OpenConversationApiRes {
        logger.info(
            "support api open: allocate request customer={} subject={}",
            request.customerActorId,
            request.subject,
        )
        val allocated = channels
            .requestToChannel(
                SampleNames.SupportChannel,
                AllocateConversationReq(
                    request.customerActorId,
                    request.customerDisplayName,
                    request.subject,
                ),
            )
            .timeout(SampleTimings.RequestTimeout)
            .submit(AllocateConversationRes::class.java)
            .await()
        logger.info(
            "support api open: allocated conversation={} status={}",
            allocated.conversationId,
            allocated.status,
        )
        return OpenConversationApiRes(allocated.conversationId, allocated.status)
    }

    private companion object {
        private val logger = LoggerFactory.getLogger(OpenConversationHandler::class.java)
    }
}
