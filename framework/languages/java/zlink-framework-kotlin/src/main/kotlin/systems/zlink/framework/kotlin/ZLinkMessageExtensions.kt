package systems.zlink.framework.kotlin

import kotlin.reflect.KClass
import systems.zlink.framework.messaging.ZLinkMessage

public inline fun <reified T : Any> messageOf(value: T): ZLinkMessage =
    ZLinkMessage.of(value, T::class.java)

public fun messageOf(value: Any, declaredType: KClass<*>): ZLinkMessage =
    ZLinkMessage.of(value, declaredType.java)

public inline fun <reified T> ZLinkMessage.decode(): T = decode(T::class.java)
