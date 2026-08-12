package systems.zlink.framework.kotlin

import systems.zlink.framework.messaging.ZLinkMessage
import kotlin.reflect.KClass

public inline fun <reified T : Any> messageOf(value: T): ZLinkMessage =
    ZLinkMessage.of(value, T::class.java)

public fun messageOf(value: Any, declaredType: KClass<*>): ZLinkMessage =
    ZLinkMessage.of(value, declaredType.java)

public inline fun <reified T> ZLinkMessage.decode(): T = decode(T::class.java)
