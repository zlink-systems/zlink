package systems.zlink.framework.perf.kotlin

import java.util.concurrent.atomic.AtomicIntegerArray
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.async
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Test

class PerfKotlinWorkloadTest {
    @Test
    fun localStreamsAdvanceOnlyAfterTheirOwnTerminal() = runBlocking {
        withTimeout(2000) {
            val entered = Array(2) { CompletableDeferred<Unit>() }
            val released = Array(2) { CompletableDeferred<Unit>() }
            val advanced = CompletableDeferred<Unit>()
            val calls = AtomicIntegerArray(2)
            val done = async {
                runTerminalStreams(2, { true }) { stream ->
                    if (calls.incrementAndGet(stream) == 1) {
                        entered[stream].complete(Unit)
                        released[stream].await()
                        true
                    } else {
                        advanced.complete(Unit)
                        false
                    }
                }
            }
            entered.forEach { it.await() }
            assertEquals(listOf(1, 1), List(2) { calls.get(it) })
            released[0].complete(Unit)
            advanced.await()
            assertEquals(listOf(2, 1), List(2) { calls.get(it) })
            released[1].complete(Unit)
            done.await()
        }
    }

    @Test
    fun streamTargetsAreAssignedForEachStreamAtSetup() {
        assertEquals(
            listOf("spot-a", "spot-b", "spot-a", "spot-b", "spot-a"),
            planStreamTargets(listOf("spot-a", "spot-b"), 5),
        )
    }
}
