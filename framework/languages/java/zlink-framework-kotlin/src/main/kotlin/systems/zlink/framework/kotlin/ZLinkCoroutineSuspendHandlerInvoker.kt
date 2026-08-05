package systems.zlink.framework.kotlin

import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Method
import java.util.concurrent.CompletionStage
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.future.future
import kotlin.coroutines.Continuation
import kotlin.coroutines.CoroutineContext
import kotlin.coroutines.intrinsics.COROUTINE_SUSPENDED
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.coroutines.suspendCoroutine
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter

internal class ZLinkCoroutineSuspendHandlerInvoker : ZLinkSuspendInvocationAdapter {
    private val scope: CoroutineScope
    private val dispatcher: CoroutineDispatcher

    @JvmOverloads
    constructor(dispatcher: CoroutineDispatcher = Dispatchers.Default) {
        this.dispatcher = dispatcher
        this.scope = object : CoroutineScope {
            override val coroutineContext: CoroutineContext = dispatcher
        }
    }

    @JvmOverloads
    constructor(
        scope: CoroutineScope,
        dispatcher: CoroutineDispatcher = Dispatchers.Default,
    ) {
        this.dispatcher = dispatcher
        this.scope = scope
    }

    override fun supports(method: Method): Boolean =
        ZLinkHandlerMethodInvoker.isKotlinSuspendMethod(method)

    @Suppress("UNCHECKED_CAST")
    override fun invoke(
        handler: Any,
        method: Method,
        logicalArguments: Array<Any>,
    ): CompletionStage<Any> {
        val invocationContext = ZLinkCoroutineInvocationContext.capture(dispatcher)
        return (
        scope.future(invocationContext) {
            invokeSuspend(handler, method, logicalArguments)
        } as CompletionStage<Any>
        )
    }

    private suspend fun invokeSuspend(
        handler: Any,
        method: Method,
        logicalArguments: Array<Any>,
    ): Any? =
        suspendCoroutine { continuation ->
            val arguments = arrayOfNulls<Any>(logicalArguments.size + 1)
            for (index in logicalArguments.indices) {
                arguments[index] = logicalArguments[index]
            }
            @Suppress("UNCHECKED_CAST")
            arguments[arguments.lastIndex] = continuation as Continuation<Any?>
            try {
                method.isAccessible = true
                val result = method.invoke(handler, *arguments)
                if (result !== COROUTINE_SUSPENDED) {
                    continuation.resume(if (result === Unit) null else result)
                }
            } catch (ex: InvocationTargetException) {
                continuation.resumeWithException(ex.targetException ?: ex)
            } catch (ex: Throwable) {
                continuation.resumeWithException(ex)
            }
        }
}
