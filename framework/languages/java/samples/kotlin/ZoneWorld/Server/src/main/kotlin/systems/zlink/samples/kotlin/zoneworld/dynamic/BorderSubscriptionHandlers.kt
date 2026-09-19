package systems.zlink.samples.kotlin.zoneworld.dynamic

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.handlers.ZLinkSpotSubscription
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler
import systems.zlink.samples.kotlin.zoneworld.server.zone.ZoneSpot
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames

class BorderSubscriptionHandlers {
    companion object {
        fun forRoute(from: String, to: String): Class<*> = when (ZoneWorldNames.borderTopic(from, to)) {
            ZoneWorldNames.NW_NE -> NorthWestToNorthEast::class.java
            ZoneWorldNames.NW_SW -> NorthWestToSouthWest::class.java
            ZoneWorldNames.NE_NW -> NorthEastToNorthWest::class.java
            ZoneWorldNames.NE_SE -> NorthEastToSouthEast::class.java
            ZoneWorldNames.SW_NW -> SouthWestToNorthWest::class.java
            ZoneWorldNames.SW_SE -> SouthWestToSouthEast::class.java
            ZoneWorldNames.SE_NE -> SouthEastToNorthEast::class.java
            ZoneWorldNames.SE_SW -> SouthEastToSouthWest::class.java
            else -> error("unknown border route")
        }
        fun apply(spot: ZoneSpot, event: Messages.ZoneBorderEvent): CompletionStage<Void> {
            spot.applyBorder(event); return CompletableFuture.completedFuture(null)
        }
    }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NW_NE) class NorthWestToNorthEast : ZLinkSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> { override fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) = apply(spot, event) }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NW_SW) class NorthWestToSouthWest : ZLinkSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> { override fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) = apply(spot, event) }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NE_NW) class NorthEastToNorthWest : ZLinkSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> { override fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) = apply(spot, event) }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NE_SE) class NorthEastToSouthEast : ZLinkSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> { override fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) = apply(spot, event) }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SW_NW) class SouthWestToNorthWest : ZLinkSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> { override fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) = apply(spot, event) }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SW_SE) class SouthWestToSouthEast : ZLinkSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> { override fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) = apply(spot, event) }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SE_NE) class SouthEastToNorthEast : ZLinkSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> { override fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) = apply(spot, event) }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SE_SW) class SouthEastToSouthWest : ZLinkSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> { override fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) = apply(spot, event) }
}
