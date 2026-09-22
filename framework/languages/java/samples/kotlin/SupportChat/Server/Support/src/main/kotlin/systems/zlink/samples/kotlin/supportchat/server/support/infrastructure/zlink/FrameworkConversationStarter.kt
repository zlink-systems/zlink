package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink

import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.support.application.ConversationStartReq
import systems.zlink.samples.kotlin.supportchat.server.support.application.ConversationStarter
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.ConversationCreateReq

class FrameworkConversationStarter(private val spots: ZLinkSpotManager) : ConversationStarter {
    private val kotlinSpots = spots.kotlin()

    override suspend fun start(conversationId: String, request: ConversationStartReq) {
        // --8<-- [start:doc-sc-api-open]
        kotlinSpots
            .getOrCreate(conversationId, SampleNames.ConversationSpotType)
            .request(
                ConversationCreateReq(
                    customerActorId = request.customerActorId,
                    customerDisplayName = request.customerDisplayName,
                    subject = request.subject,
                    createdAtUnixMs = request.createdAtUnixMs,
                )
            )
            .await()
        // --8<-- [end:doc-sc-api-open]
    }
}
