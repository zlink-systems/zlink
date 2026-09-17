package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers

import systems.zlink.framework.handlers.ZLinkSpotSubscription
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotSubscriptionHandler
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot.PlayEntrySpot
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerWinMilestoneEvent

// --8<-- [start:doc-multicast-subscribe]
@ZLinkSpotSubscription(topic = SampleNames.PlayerMilestoneTopic)
class PlayerWinMilestoneEventHandler :
    ZLinkSuspendingSpotSubscriptionHandler<PlayEntrySpot, PlayerWinMilestoneEvent> {
    override suspend fun handle(spot: PlayEntrySpot, event: PlayerWinMilestoneEvent) {
        spot.notifyMilestone(event)
    }
}
// --8<-- [end:doc-multicast-subscribe]
