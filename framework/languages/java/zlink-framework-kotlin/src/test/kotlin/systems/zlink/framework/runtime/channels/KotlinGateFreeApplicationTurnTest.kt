package systems.zlink.framework.runtime.channels

import java.util.concurrent.CompletableFuture
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine
import kotlinx.coroutines.Dispatchers
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertNotNull
import org.junit.jupiter.api.Assertions.assertSame
import org.junit.jupiter.api.Assertions.assertThrows
import org.junit.jupiter.api.Test
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkCoroutineSuspendHandlerInvoker
import systems.zlink.framework.monitoring.ZLinkFlowOrigin
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker
import systems.zlink.framework.runtime.internal.calls.ZLinkBlockingCalls
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext

class KotlinGateFreeApplicationTurnTest {
    @Test
    fun suspendingGateFreeHandlerKeepsItsRecordContextWhileNextRecordRuns() {
        Executors.newFixedThreadPool(2).use { workers ->
            val owner = ZLinkApplicationExecutionScheduler(workers, 2).register()
            val handler = SuspendingHandler()
            val invocation =
                ZLinkHandlerMethodInvoker.prepareChannel(
                    SuspendingHandler::class.java,
                    null,
                    String::class.java,
                    ZLinkMessageContext::class.java,
                    listOf(ZLinkCoroutineSuspendHandlerInvoker(Dispatchers.Unconfined)),
                )
            val first =
                ZLinkFlowContext.enterOrCreate(null, ZLinkFlowOrigin.APPLICATION).use {
                    handler.expectedFlow = ZLinkFlowContext.current().flowId()
                    owner
                        .enqueue({ invocation.invoke(handler, "first", null).thenAccept {} }, null)
                        .toCompletableFuture()
                }
            try {
                handler.entered.get(2, TimeUnit.SECONDS)
                owner
                    .enqueue({ CompletableFuture.completedFuture(null) }, null)
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS)
                assertFalse(first.isDone)
                checkNotNull(handler.suspended.getAndSet(null)).resume(Unit)
                first.get(2, TimeUnit.SECONDS)
                assertSame(handler.before.get(), handler.after.get())
            } finally {
                handler.suspended.getAndSet(null)?.resume(Unit)
            }
        }
    }

    class SuspendingHandler {
        var expectedFlow: String? = null
        val entered = CompletableFuture<Void>()
        val suspended = AtomicReference<Continuation<Unit>?>()
        val before = AtomicReference<Any?>()
        val after = AtomicReference<Any?>()

        @Suppress("UNUSED_PARAMETER")
        suspend fun handle(message: String, context: ZLinkMessageContext?): String {
            before.set(ZLinkSuspendInvocationContext.currentSerialExecutionTurn())
            assertNotNull(before.get())
            assertEquals(expectedFlow, ZLinkFlowContext.current().flowId())
            assertThrows(RuntimeException::class.java) { ZLinkBlockingCalls.requireMayBlock() }
            suspendCoroutine<Unit> {
                suspended.set(it)
                entered.complete(null)
            }
            after.set(ZLinkSuspendInvocationContext.currentSerialExecutionTurn())
            assertEquals(expectedFlow, ZLinkFlowContext.current().flowId())
            assertThrows(RuntimeException::class.java) { ZLinkBlockingCalls.requireMayBlock() }
            return message
        }
    }
}
