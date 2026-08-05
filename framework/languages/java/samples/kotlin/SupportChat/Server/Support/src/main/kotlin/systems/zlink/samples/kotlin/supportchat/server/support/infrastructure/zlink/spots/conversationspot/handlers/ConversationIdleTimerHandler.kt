package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.handlers

import systems.zlink.framework.kotlin.ZLinkSuspendingSpotTimerHandler
import systems.zlink.framework.spots.ZLinkTimerTick
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.ConversationSpot

class ConversationIdleTimerHandler : ZLinkSuspendingSpotTimerHandler<ConversationSpot> {
    override suspend fun handle(
        spot: ConversationSpot,
        tick: ZLinkTimerTick,
    ) {
        spot.checkIdle()
    }
}
