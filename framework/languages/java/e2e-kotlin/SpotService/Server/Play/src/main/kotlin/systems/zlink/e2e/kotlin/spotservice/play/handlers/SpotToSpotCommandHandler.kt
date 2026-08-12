package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.play.spots.UserSpot
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotPacketHandler

class SpotToSpotCommandHandler : ZLinkSuspendingSpotPacketHandler<UserSpot, Contracts.SpotToSpotMsg> {
    override suspend fun handle(spot: UserSpot, message: Contracts.SpotToSpotMsg) {
        spot.context()
            .outbound()
            .sendToSpot(
                message.targetSpotRid,
                Contracts.OutboundMsg(message.value),
            )
            .submit()
    }
}
