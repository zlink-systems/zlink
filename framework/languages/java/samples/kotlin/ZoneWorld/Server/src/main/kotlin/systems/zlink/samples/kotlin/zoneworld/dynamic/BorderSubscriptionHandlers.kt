package systems.zlink.samples.kotlin.zoneworld.dynamic

import systems.zlink.framework.handlers.ZLinkSpotSubscription
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotSubscriptionHandler
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry
import systems.zlink.samples.kotlin.zoneworld.server.zone.ZoneSpot
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames

class BorderSubscriptionHandlers {
    companion object {
        fun registerForRoute(
            handlers: ZLinkSpotHandlerRegistry,
            fromZoneId: String,
            toZoneId: String,
        ) {
            when (ZoneWorldNames.borderTopic(fromZoneId, toZoneId)) {
                ZoneWorldNames.NW_NE -> handlers.addHandler<NorthWestToNorthEast>()
                ZoneWorldNames.NW_SW -> handlers.addHandler<NorthWestToSouthWest>()
                ZoneWorldNames.NE_NW -> handlers.addHandler<NorthEastToNorthWest>()
                ZoneWorldNames.NE_SE -> handlers.addHandler<NorthEastToSouthEast>()
                ZoneWorldNames.SW_NW -> handlers.addHandler<SouthWestToNorthWest>()
                ZoneWorldNames.SW_SE -> handlers.addHandler<SouthWestToSouthEast>()
                ZoneWorldNames.SE_NE -> handlers.addHandler<SouthEastToNorthEast>()
                ZoneWorldNames.SE_SW -> handlers.addHandler<SouthEastToSouthWest>()
                else -> error("unknown border route: $fromZoneId -> $toZoneId")
            }
        }

        suspend fun apply(spot: ZoneSpot, event: Messages.ZoneBorderEvent) {
            spot.applyBorder(event)
        }
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.NW_NE)
    class NorthWestToNorthEast :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.NW_SW)
    class NorthWestToSouthWest :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.NE_NW)
    class NorthEastToNorthWest :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.NE_SE)
    class NorthEastToSouthEast :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.SW_NW)
    class SouthWestToNorthWest :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.SW_SE)
    class SouthWestToSouthEast :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.SE_NE)
    class SouthEastToNorthEast :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.SE_SW)
    class SouthEastToSouthWest :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }
}
