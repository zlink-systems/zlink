package systems.zlink.framework.kotlin

import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertNull
import org.junit.jupiter.api.Assertions.assertSame
import org.junit.jupiter.api.Test
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext

class KotlinInvocationContextElementTest {
    @Test
    fun `suspend invocation carries one context element besides the dispatcher`() {
        val marker = Any()
        val context =
            ZLinkSuspendInvocationContext.enterEntrySpotDispatch(marker).use {
                ZLinkCoroutineInvocationContext.capture(Dispatchers.Default)
            }

        val elements = context.fold(0) { count, _ -> count + 1 }

        assertEquals(
            2,
            elements,
            "dispatcher plus one invocation element, not one per thread-local",
        )
    }

    @Test
    fun `invocation state follows the coroutine across threads and leaves them clean`() {
        val first = Executors.newSingleThreadExecutor { Thread(it, "invocation-a") }
        val second = Executors.newSingleThreadExecutor { Thread(it, "invocation-b") }
        try {
            val marker = Any()
            val actor = "actor-a"
            val context =
                ZLinkSuspendInvocationContext.enterEntrySpotDispatch(marker).use {
                    ZLinkSuspendInvocationContext.enterActorDispatch(actor).use {
                        ZLinkCoroutineInvocationContext.capture(first.asCoroutineDispatcher())
                    }
                }
            // The capturing thread was restored when the scopes closed.
            assertNull(ZLinkSuspendInvocationContext.currentEntrySpotDispatch())

            val firstThread = first.submit<Thread> { Thread.currentThread() }.get()
            val secondThread = second.submit<Thread> { Thread.currentThread() }.get()
            val seen = mutableListOf<Triple<Thread, Any?, String?>>()
            runBlocking(context) {
                seen +=
                    Triple(
                        Thread.currentThread(),
                        ZLinkSuspendInvocationContext.currentEntrySpotDispatch(),
                        ZLinkSuspendInvocationContext.currentActorDispatch(),
                    )
                withContext(second.asCoroutineDispatcher()) {
                    seen +=
                        Triple(
                            Thread.currentThread(),
                            ZLinkSuspendInvocationContext.currentEntrySpotDispatch(),
                            ZLinkSuspendInvocationContext.currentActorDispatch(),
                        )
                }
                seen +=
                    Triple(
                        Thread.currentThread(),
                        ZLinkSuspendInvocationContext.currentEntrySpotDispatch(),
                        ZLinkSuspendInvocationContext.currentActorDispatch(),
                    )
            }

            assertSame(firstThread, seen[0].first)
            assertSame(secondThread, seen[1].first)
            assertSame(firstThread, seen[2].first)
            for ((_, spot, dispatchedActor) in seen) {
                assertSame(marker, spot)
                assertEquals(actor, dispatchedActor)
            }
            for (executor in listOf(first, second)) {
                val leftover =
                    executor
                        .submit<Pair<Any?, String?>> {
                            ZLinkSuspendInvocationContext.currentEntrySpotDispatch() to
                                ZLinkSuspendInvocationContext.currentActorDispatch()
                        }
                        .get(3, TimeUnit.SECONDS)
                assertNull(leftover.first)
                assertNull(leftover.second)
            }
        } finally {
            first.shutdownNow()
            second.shutdownNow()
        }
    }
}
