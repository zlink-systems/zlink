package systems.zlink.framework.kotlin

import systems.zlink.framework.handlers.ZLinkSpotSubscription
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry
import systems.zlink.framework.spots.ZLinkInstanceSpotHandlerRegistry
import systems.zlink.framework.spots.ZLinkSpotPacketHandler
import systems.zlink.framework.spots.ZLinkSpotRequestHandler
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler

inline fun <reified THandler : Any> ZLinkSpotHandlerRegistry.addHandler() {
    addTypedHandler(THandler::class.java)
}

inline fun <reified THandler : Any> ZLinkInstanceSpotHandlerRegistry.addHandler() {
    addPacket(THandler::class.java)
}

@PublishedApi
internal fun ZLinkSpotHandlerRegistry.addTypedHandler(handlerType: Class<*>) {
    when {
        handlerType.hasInterface<ZLinkSpotSubscriptionHandler<*, *>>()
            || handlerType.hasInterface<ZLinkSuspendingSpotSubscriptionHandler<*, *>>() -> {
            if (handlerType.spotSubscriptionTopic() == null) {
                throw IllegalArgumentException(
                    "SPOT subscription handler must declare @ZLinkSpotSubscription(topic = ...): "
                        + handlerType.name)
            }
            addHandler(handlerType)
        }
        handlerType.hasInterface<ZLinkSpotPacketHandler<*, *>>()
            || handlerType.hasInterface<ZLinkSpotRequestHandler<*, *, *>>()
            || handlerType.hasInterface<ZLinkSuspendingSpotPacketHandler<*, *>>()
            || handlerType.hasInterface<ZLinkSuspendingSpotRequestHandler<*, *, *>>() -> {
            addHandler(handlerType)
        }
        handlerType.hasInterface<ZLinkEntrySpotActorSendHandler<*, *, *>>()
            || handlerType.hasInterface<ZLinkEntrySpotActorRequestHandler<*, *, *, *>>()
            || handlerType.hasInterface<ZLinkSpotActorSendHandler<*, *, *>>()
            || handlerType.hasInterface<ZLinkSpotActorRequestHandler<*, *, *, *>>()
            || handlerType.hasInterface<ZLinkSuspendingEntrySpotActorSendHandler<*, *, *>>()
            || handlerType.hasInterface<ZLinkSuspendingEntrySpotActorRequestHandler<*, *, *, *>>()
            || handlerType.hasInterface<ZLinkSuspendingSpotActorSendHandler<*, *, *>>()
            || handlerType.hasInterface<ZLinkSuspendingSpotActorRequestHandler<*, *, *, *>>() -> {
            addHandler(handlerType)
        }
        else -> addHandler(handlerType)
    }
}

private inline fun <reified TInterface> Class<*>.hasInterface(): Boolean {
    return TInterface::class.java.isAssignableFrom(this)
}

private fun Class<*>.spotSubscriptionTopic(): String? {
    val classAnnotation = getAnnotation(ZLinkSpotSubscription::class.java)
    if (classAnnotation != null) {
        return classAnnotation.topic
    }
    return methods
        .firstNotNullOfOrNull { method ->
            method.getAnnotation(ZLinkSpotSubscription::class.java)?.topic
        }
}
