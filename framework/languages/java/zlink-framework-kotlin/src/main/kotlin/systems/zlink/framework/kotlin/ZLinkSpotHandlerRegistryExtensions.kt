@file:Suppress("ForbiddenMethodCall") // Kotlin adapter owns the Java Class registration bridge.

package systems.zlink.framework.kotlin

import java.time.Duration
import java.util.concurrent.CompletionStage
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkInstanceSpotContext
import systems.zlink.framework.spots.ZLinkInstanceSpotHandlerRegistry
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry
import systems.zlink.framework.spots.ZLinkTimer
import systems.zlink.framework.spots.ZLinkTimerOptions

inline fun <reified THandler : Any> ZLinkSpotHandlerRegistry.addHandler() {
    addHandler(THandler::class.java)
}

inline fun <reified THandler : Any> ZLinkInstanceSpotHandlerRegistry.addPacket() {
    addPacket(THandler::class.java)
}

inline fun <reified THandler : Any> ZLinkEntrySpotContext.addTimer(
    name: String,
    period: Duration,
    options: ZLinkTimerOptions,
): CompletionStage<ZLinkTimer> = addTimer(name, period, THandler::class.java, options)

inline fun <reified THandler : Any> ZLinkSpotContext.addTimer(
    name: String,
    period: Duration,
    options: ZLinkTimerOptions,
): CompletionStage<ZLinkTimer> = addTimer(name, period, THandler::class.java, options)

inline fun <reified THandler : Any> ZLinkInstanceSpotContext.addTimer(
    name: String,
    period: Duration,
    options: ZLinkTimerOptions,
): CompletionStage<ZLinkTimer> = addTimer(name, period, THandler::class.java, options)
