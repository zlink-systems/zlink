package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers

import systems.zlink.framework.handlers.ZLinkSpotSubscription
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotSubscriptionHandler
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRewardAcquiredEvent

@ZLinkSpotSubscription(topic = SampleNames.WinnerTopic)
class BingoRewardAcquiredEventHandler :
    ZLinkSuspendingSpotSubscriptionHandler<BingoRoomSpot, BingoRewardAcquiredEvent> {
    override suspend fun handle(spot: BingoRoomSpot, event: BingoRewardAcquiredEvent) {
        spot.announceReward(event)
    }
}
