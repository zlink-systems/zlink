package systems.zlink.framework.kotlin

import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Test

class ZLinkOwnedCallbackFlowTest {
    @Test
    fun cancellationBeforeCollectorAcceptsReleasesCurrentAndQueuedItemsOnce() = runBlocking {
        val registered = CompletableDeferred<(Int) -> Unit>()
        val beforeCollector = CompletableDeferred<Unit>()
        val released = ConcurrentHashMap<Int, AtomicInteger>()
        val collector =
            launch(start = CoroutineStart.UNDISPATCHED) {
                ownedCallbackFlow<Int>(
                        register = { accept ->
                            registered.complete(accept)
                            AutoCloseable {}
                        },
                        release = { item ->
                            released.computeIfAbsent(item) { AtomicInteger() }.incrementAndGet()
                        },
                    )
                    .onEach {
                        beforeCollector.complete(Unit)
                        awaitCancellation()
                    }
                    .collect { error("collector received an item after cancellation") }
            }

        val accept = registered.await()
        accept(1)
        accept(2)
        beforeCollector.await()
        collector.cancelAndJoin()

        assertEquals(1, released[1]?.get(), "item in emit must be released")
        assertEquals(1, released[2]?.get(), "queued item must be released once")
    }
}
