package systems.zlink.framework.kotlin

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import systems.zlink.framework.configuration.ZLinkFrameworkOptions
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendHandlerOptions

private fun ZLinkFrameworkOptions.suspendHandlerOptions(): ZLinkSuspendHandlerOptions =
    this as? ZLinkSuspendHandlerOptions
        ?: error("The configured Framework options do not support Kotlin coroutine handlers")

fun ZLinkFrameworkOptions.useCoroutineHandlers(dispatcher: CoroutineDispatcher) {
    suspendHandlerOptions().useSuspendHandlerInvoker(
        ZLinkCoroutineSuspendHandlerInvoker(dispatcher),
    )
}

fun ZLinkFrameworkOptions.useCoroutineHandlers(
    scope: CoroutineScope,
    dispatcher: CoroutineDispatcher,
) {
    suspendHandlerOptions().useSuspendHandlerInvoker(
        ZLinkCoroutineSuspendHandlerInvoker(scope, dispatcher),
    )
}
