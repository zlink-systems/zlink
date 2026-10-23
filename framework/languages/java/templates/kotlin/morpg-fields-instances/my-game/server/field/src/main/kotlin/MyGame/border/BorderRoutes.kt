package MyGame.border

import MyGame.*
import MyGame.field.Field
import systems.zlink.framework.handlers.ZLinkSpotSubscription
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry

object BorderTopics {
    const val FIELD1_TO2 = "field-1:field-2"
    const val FIELD2_TO1 = "field-2:field-1"
    const val FIELD2_TO3 = "field-2:field-3"
    const val FIELD3_TO2 = "field-3:field-2"
    const val FIELD3_TO4 = "field-3:field-4"
    const val FIELD4_TO3 = "field-4:field-3"
}

// Annotation topics are compile-time constants. Registration follows the public ZoneWorld sample.
object BorderRoutes {
    fun register(handlers: ZLinkSpotHandlerRegistry, source: String, target: String) {
        when (Rules.borderTopic(source, target)) {
            BorderTopics.FIELD1_TO2 -> handlers.addHandler<Route1To2>()
            BorderTopics.FIELD2_TO1 -> handlers.addHandler<Route2To1>()
            BorderTopics.FIELD2_TO3 -> handlers.addHandler<Route2To3>()
            BorderTopics.FIELD3_TO2 -> handlers.addHandler<Route3To2>()
            BorderTopics.FIELD3_TO4 -> handlers.addHandler<Route3To4>()
            BorderTopics.FIELD4_TO3 -> handlers.addHandler<Route4To3>()
            else -> error("Unknown border route: $source -> $target")
        }
    }
}

@ZLinkSpotSubscription(channelName = Names.BORDER, topic = BorderTopics.FIELD1_TO2)
class Route1To2 : ZLinkSuspendingSpotSubscriptionHandler<Field, BorderPresence> {
    override suspend fun handle(spot: Field, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(channelName = Names.BORDER, topic = BorderTopics.FIELD2_TO1)
class Route2To1 : ZLinkSuspendingSpotSubscriptionHandler<Field, BorderPresence> {
    override suspend fun handle(spot: Field, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(channelName = Names.BORDER, topic = BorderTopics.FIELD2_TO3)
class Route2To3 : ZLinkSuspendingSpotSubscriptionHandler<Field, BorderPresence> {
    override suspend fun handle(spot: Field, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(channelName = Names.BORDER, topic = BorderTopics.FIELD3_TO2)
class Route3To2 : ZLinkSuspendingSpotSubscriptionHandler<Field, BorderPresence> {
    override suspend fun handle(spot: Field, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(channelName = Names.BORDER, topic = BorderTopics.FIELD3_TO4)
class Route3To4 : ZLinkSuspendingSpotSubscriptionHandler<Field, BorderPresence> {
    override suspend fun handle(spot: Field, event: BorderPresence) = spot.showBorder(event)
}

@ZLinkSpotSubscription(channelName = Names.BORDER, topic = BorderTopics.FIELD4_TO3)
class Route4To3 : ZLinkSuspendingSpotSubscriptionHandler<Field, BorderPresence> {
    override suspend fun handle(spot: Field, event: BorderPresence) = spot.showBorder(event)
}
