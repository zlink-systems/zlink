package MyGame.border

import MyGame.*
import MyGame.field.Field
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry

object BorderRoutes {
    fun register(handlers: ZLinkSpotHandlerRegistry, source: String, target: String) {
        handlers.addSubscribe(BorderHandler::class.java, Names.BORDER, Rules.borderTopic(source, target))
    }
}

class BorderHandler : ZLinkSuspendingSpotSubscriptionHandler<Field, BorderPresence> {
    override suspend fun handle(spot: Field, event: BorderPresence) = spot.showBorder(event)
}
