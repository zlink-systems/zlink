package systems.zlink.framework.kotlin

import systems.zlink.framework.messaging.ZLinkMessage

public fun messageOf(value: Any): ZLinkMessage = ZLinkMessage.of(value)

public inline fun <reified T> ZLinkMessage.decode(): T = decode(T::class.java)
