package systems.zlink.framework.execution

import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test

/** Specifies the ownership guarantees component authors may rely on from [ZLinkStateLane]. */
class ZLinkStateLaneTest {
    @Test
    fun `run returns the result of the work`() = runBlocking {
        val lane = ZLinkStateLane()
        try {
            assertEquals(42, lane.run { 42 })
        } finally {
            lane.closeAndJoin()
        }
    }

    @Test
    fun `run surfaces a failure to its own caller`() = runBlocking {
        val lane = ZLinkStateLane()
        try {
            val error = failureFrom { lane.run<Int> { throw IllegalStateException("boom") } }

            assertTrue(error is IllegalStateException)
            assertEquals("boom", error.message)
        } finally {
            lane.closeAndJoin()
        }
    }

    @Test
    fun `run keeps serving after a work item throws`() = runBlocking {
        val lane = ZLinkStateLane()
        try {
            val error = failureFrom { lane.run<Int> { throw IllegalStateException() } }

            assertTrue(error is IllegalStateException)
            assertEquals(7, lane.run { 7 })
        } finally {
            lane.closeAndJoin()
        }
    }

    @Test
    fun `concurrent callers mutate unsynchronized state without losing updates`() = runBlocking {
        val lane = ZLinkStateLane()
        val state = mutableMapOf<Int, Int>()
        val callers = 32
        val perCaller = 50
        try {
            coroutineScope {
                (0 until callers).map { caller ->
                    launch(Dispatchers.Default) {
                        repeat(perCaller) { index ->
                            val key = caller * perCaller + index
                            lane.run { state[key] = key }
                        }
                    }
                }.forEach { it.join() }
            }

            assertEquals(callers * perCaller, lane.run { state.size })
        } finally {
            lane.closeAndJoin()
        }
    }

    @Test
    fun `work items never overlap`() = runBlocking {
        val lane = ZLinkStateLane()
        val inFlight = AtomicInteger()
        val observedOverlap = AtomicBoolean(false)
        try {
            coroutineScope {
                (0 until 64).map {
                    async(Dispatchers.Default) {
                        lane.run {
                            if (inFlight.incrementAndGet() != 1) {
                                observedOverlap.set(true)
                            }
                            repeat(200) { Thread.onSpinWait() }
                            inFlight.decrementAndGet()
                        }
                    }
                }.awaitAll()
            }

            assertFalse(observedOverlap.get())
        } finally {
            lane.closeAndJoin()
        }
    }

    @Test
    fun `posts from one caller run in post order`() = runBlocking {
        val lane = ZLinkStateLane()
        val order = mutableListOf<Int>()
        try {
            repeat(100) { value ->
                assertTrue(lane.tryPost { order += value })
            }

            assertEquals((0 until 100).toList(), lane.run { order.toList() })
        } finally {
            lane.closeAndJoin()
        }
    }

    @Test
    fun `draining more than one batch still runs every item`() = runBlocking {
        val lane = ZLinkStateLane()
        var count = 0
        try {
            repeat(250) {
                assertTrue(lane.tryPost { count++ })
            }

            assertEquals(250, lane.run { count })
        } finally {
            lane.closeAndJoin()
        }
    }

    @Test
    fun `reentering the same lane fails instead of hanging`() = runBlocking {
        val lane = ZLinkStateLane()
        try {
            val error = withTimeout(3_000) {
                lane.run {
                    failureFrom { lane.run { 1 } }
                }
            }

            assertTrue(error.message!!.contains("already runs on the state lane"))
        } finally {
            lane.closeAndJoin()
        }
    }

    @Test
    fun `is on lane is true only inside a turn`() = runBlocking {
        val lane = ZLinkStateLane()
        try {
            assertFalse(lane.isOnLane())
            assertTrue(lane.run { lane.isOnLane() })
            assertFalse(lane.isOnLane())
        } finally {
            lane.closeAndJoin()
        }
    }

    @Test
    fun `a different lane is enterable from inside a turn`() = runBlocking {
        val outer = ZLinkStateLane()
        val inner = ZLinkStateLane()
        try {
            assertEquals(5, outer.run { inner.run { 5 } })
        } finally {
            outer.closeAndJoin()
            inner.closeAndJoin()
        }
    }

    @Test
    fun `close and join waits for queued work`() = runBlocking {
        val lane = ZLinkStateLane()
        var completed = 0

        repeat(200) {
            assertTrue(lane.tryPost { completed++ })
        }
        lane.closeAndJoin()

        assertEquals(200, completed)
    }

    @Test
    fun `run after close and join throws`() = runBlocking {
        val lane = ZLinkStateLane()
        lane.closeAndJoin()

        val error = failureFrom { lane.run { 1 } }

        assertTrue(error is IllegalStateException)
    }

    @Test
    fun `try post after close and join reports refusal instead of throwing`() = runBlocking {
        val lane = ZLinkStateLane()
        lane.closeAndJoin()

        assertFalse(lane.tryPost { })
    }

    @Test
    fun `close and join is idempotent`() = runBlocking {
        val lane = ZLinkStateLane()

        lane.closeAndJoin()
        lane.closeAndJoin()
    }

    private suspend fun failureFrom(block: suspend () -> Unit): Throwable {
        try {
            block()
        } catch (error: Throwable) {
            return error
        }
        error("Expected the operation to fail.")
    }
}
