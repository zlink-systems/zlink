package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.handlers

import systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorRequestHandler
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActor
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.ConversationSpot
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SendChatMessageReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SendChatMessageRes

class SendChatMessageHandler : ZLinkSuspendingSpotActorRequestHandler<
    ConversationSpot,
    SupportUserActor,
    SendChatMessageReq,
    SendChatMessageRes,
    > {
    override suspend fun handle(
        spot: ConversationSpot,
        actor: SupportUserActor,
        context: ZLinkMessageContext,
        request: SendChatMessageReq,
    ): SendChatMessageRes = spot.sendMessage(actor, request)
}
