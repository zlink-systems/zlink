package systems.zlink.framework.kotlin

import java.util.OptionalLong
import java.util.concurrent.atomic.AtomicReference
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine
import kotlinx.coroutines.Dispatchers
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue

class KotlinSuspendPermitBoundaryTest {
    @Test
    fun actualSuspendEntryReturnsPermitAndResumeCannotReleaseTheNextJob() {
        val jobs =
            ZLinkApplicationJobQueue(
                ZLinkApplicationJobQueueProfile.BALANCED,
                OptionalLong.of(1),
                ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null),
            )
        val permit =
            jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE).toCompletableFuture().join()
        permit.queued()
        val handler = SuspendingHandler(jobs)
        val invocation =
            ZLinkHandlerMethodInvoker.prepareChannel(
                SuspendingHandler::class.java,
                null,
                String::class.java,
                ZLinkMessageContext::class.java,
                listOf(ZLinkCoroutineSuspendHandlerInvoker(Dispatchers.Unconfined)),
            )
        try {
            val result =
                ZLinkApplicationJobContext.enterQueued(permit).use {
                    invocation.invoke(handler, "payload", null).toCompletableFuture()
                }
            assertFalse(result.isDone)
            assertEquals(0, jobs.snapshot().permitsInUse())
            val next = jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE).toCompletableFuture()
            assertTrue(next.isDone)
            val nextPermit = next.join()
            try {
                assertEquals(1, jobs.snapshot().permitsInUse())
                checkNotNull(handler.suspended.getAndSet(null)).resume(Unit)
                assertEquals("payload", result.join())
                assertEquals(1, jobs.snapshot().permitsInUse())
            } finally {
                nextPermit.close()
            }
        } finally {
            handler.suspended.getAndSet(null)?.resume(Unit)
            permit.close()
            jobs.close()
        }
    }

    class SuspendingHandler(private val jobs: ZLinkApplicationJobQueue) {
        val suspended = AtomicReference<Continuation<Unit>?>()

        @Suppress("UNUSED_PARAMETER")
        suspend fun handle(message: String, context: ZLinkMessageContext?): String {
            assertEquals(0, jobs.snapshot().permitsInUse())
            suspendCoroutine<Unit> { suspended.set(it) }
            return message
        }
    }
}
