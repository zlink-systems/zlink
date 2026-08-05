package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink

import kotlinx.coroutines.future.await
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.samples.kotlin.supportchat.server.support.application.ConversationStartReq
import systems.zlink.samples.kotlin.supportchat.server.support.application.ConversationStarter
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.ConversationCreateReq
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.ConversationSpot
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames

class FrameworkConversationStarter(
    private val spots: ZLinkSpotManager,
) : ConversationStarter {
    override suspend fun start(
        conversationId: String,
        request: ConversationStartReq,
    ) {
        spots.getOrCreate(conversationId, SampleNames.ConversationSpotType)
            .request(
                ConversationCreateReq(
                    customerActorId = request.customerActorId,
                    customerDisplayName = request.customerDisplayName,
                    subject = request.subject,
                    createdAtUnixMs = request.createdAtUnixMs,
                ),
            )
            .submit()
            .await()
    }
}
