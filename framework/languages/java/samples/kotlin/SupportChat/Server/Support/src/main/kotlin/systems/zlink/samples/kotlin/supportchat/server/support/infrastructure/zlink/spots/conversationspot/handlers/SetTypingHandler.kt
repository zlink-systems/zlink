package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.handlers

import systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorSendHandler
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActor
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.ConversationSpot
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SetTypingReq

class SetTypingHandler : ZLinkSuspendingSpotActorSendHandler<
    ConversationSpot,
    SupportUserActor,
    SetTypingReq,
    > {
    override suspend fun handle(
        spot: ConversationSpot,
        actor: SupportUserActor,
        context: ZLinkMessageContext,
        message: SetTypingReq,
    ) {
        spot.setTyping(actor, message)
    }
}
