package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.handlers

import systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorRequestHandler
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActor
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.ConversationSpot
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationRes

class JoinConversationHandler : ZLinkSuspendingSpotActorRequestHandler<
    ConversationSpot,
    SupportUserActor,
    JoinConversationReq,
    JoinConversationRes,
    > {
    override suspend fun handle(
        spot: ConversationSpot,
        actor: SupportUserActor,
        context: ZLinkMessageContext,
        request: JoinConversationReq,
    ): JoinConversationRes = spot.refreshMembership(actor)
}
