package MyGame.border

import MyGame.*
import MyGame.zone.Zone
import systems.zlink.framework.handlers.ZLinkSpotSubscription
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry

object BorderTopics {
    const val ZONE1_TO2 = "zone-1:zone-2"
    const val ZONE2_TO1 = "zone-2:zone-1"
    const val ZONE2_TO3 = "zone-2:zone-3"
    const val ZONE3_TO2 = "zone-3:zone-2"
    const val ZONE3_TO4 = "zone-3:zone-4"
    const val ZONE4_TO3 = "zone-4:zone-3"
    const val ZONE4_TO5 = "zone-4:zone-5"
    const val ZONE5_TO4 = "zone-5:zone-4"
    const val ZONE5_TO6 = "zone-5:zone-6"
    const val ZONE6_TO5 = "zone-6:zone-5"
}

// Annotation topics are compile-time constants. Registration follows the public ZoneWorld sample.
object BorderRoutes {
    fun register(handlers: ZLinkSpotHandlerRegistry, source: String, target: String) {
        when (Rules.borderTopic(source, target)) {
            BorderTopics.ZONE1_TO2 -> handlers.addHandler<Route1To2>()
            BorderTopics.ZONE2_TO1 -> handlers.addHandler<Route2To1>()
            BorderTopics.ZONE2_TO3 -> handlers.addHandler<Route2To3>()
            BorderTopics.ZONE3_TO2 -> handlers.addHandler<Route3To2>()
            BorderTopics.ZONE3_TO4 -> handlers.addHandler<Route3To4>()
            BorderTopics.ZONE4_TO3 -> handlers.addHandler<Route4To3>()
            BorderTopics.ZONE4_TO5 -> handlers.addHandler<Route4To5>()
            BorderTopics.ZONE5_TO4 -> handlers.addHandler<Route5To4>()
            BorderTopics.ZONE5_TO6 -> handlers.addHandler<Route5To6>()
            BorderTopics.ZONE6_TO5 -> handlers.addHandler<Route6To5>()
            else -> error("Unknown border route: $source -> $target")
        }
    }
}

@ZLinkSpotSubscription(topic = BorderTopics.ZONE1_TO2)
class Route1To2 : ZLinkSuspendingSpotSubscriptionHandler<Zone, BorderPresence> {
    override suspend fun handle(spot: Zone, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(topic = BorderTopics.ZONE2_TO1)
class Route2To1 : ZLinkSuspendingSpotSubscriptionHandler<Zone, BorderPresence> {
    override suspend fun handle(spot: Zone, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(topic = BorderTopics.ZONE2_TO3)
class Route2To3 : ZLinkSuspendingSpotSubscriptionHandler<Zone, BorderPresence> {
    override suspend fun handle(spot: Zone, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(topic = BorderTopics.ZONE3_TO2)
class Route3To2 : ZLinkSuspendingSpotSubscriptionHandler<Zone, BorderPresence> {
    override suspend fun handle(spot: Zone, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(topic = BorderTopics.ZONE3_TO4)
class Route3To4 : ZLinkSuspendingSpotSubscriptionHandler<Zone, BorderPresence> {
    override suspend fun handle(spot: Zone, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(topic = BorderTopics.ZONE4_TO3)
class Route4To3 : ZLinkSuspendingSpotSubscriptionHandler<Zone, BorderPresence> {
    override suspend fun handle(spot: Zone, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(topic = BorderTopics.ZONE4_TO5)
class Route4To5 : ZLinkSuspendingSpotSubscriptionHandler<Zone, BorderPresence> {
    override suspend fun handle(spot: Zone, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(topic = BorderTopics.ZONE5_TO4)
class Route5To4 : ZLinkSuspendingSpotSubscriptionHandler<Zone, BorderPresence> {
    override suspend fun handle(spot: Zone, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(topic = BorderTopics.ZONE5_TO6)
class Route5To6 : ZLinkSuspendingSpotSubscriptionHandler<Zone, BorderPresence> {
    override suspend fun handle(spot: Zone, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(topic = BorderTopics.ZONE6_TO5)
class Route6To5 : ZLinkSuspendingSpotSubscriptionHandler<Zone, BorderPresence> {
    override suspend fun handle(spot: Zone, event: BorderPresence) = spot.showBorder(event)
}
