package systems.zlink.framework.runtime.spots

import java.time.Duration
import java.util.concurrent.AbstractExecutorService
import java.util.concurrent.CompletableFuture
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.future.await
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.params.ParameterizedTest
import org.junit.jupiter.params.provider.ValueSource
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue
import systems.zlink.framework.execution.ZLinkWorkerPool
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotContext

class KotlinLifecycleAwaitTimerTest {
    private val completionSeconds = 5L

    @ParameterizedTest
    @ValueSource(booleans = [false, true])
    fun joinedSuspendAwaitRetainsGateUnlessExplicitlyYielded(yield: Boolean) {
        val tasks = LinkedBlockingQueue<Runnable>()
        val executor =
            object : AbstractExecutorService() {
                override fun execute(task: Runnable) {
                    tasks.add(task)
                }

                override fun shutdown() {}

                override fun shutdownNow(): MutableList<Runnable> = mutableListOf()

                override fun isShutdown() = false

                override fun isTerminated() = false

                override fun awaitTermination(value: Long, unit: TimeUnit) = true
            }
        val queue = ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot())
        val serials =
            ZLinkSpotSerialExecutor(
                queue,
                executor,
                executor,
                ZLinkUserSpotExecutionMode.SPOT_WIDE,
                false,
            )
        val entered = CompletableFuture<Void>()
        val release = CompletableFuture<Boolean>()
        val callbackDone = CompletableFuture<Void>()
        val timerEntered = AtomicBoolean()
        val pool = ZLinkWorkerPool(0, 1, Duration.ofSeconds(30))
        val actor =
            object : ZLinkActor {
                override fun context(): ZLinkActorContext = error("Actor context is unused")
            }
        val spot =
            object : ZLinkSuspendingSpot<ZLinkActor>() {
                override val context: ZLinkSpotContext
                    get() = error("Spot context is unused")

                override suspend fun onActorJoinSuspending(actorId: String, request: ZLinkMessage) =
                    ZLinkSpotActorJoinResult.accept()

                override suspend fun onJoinedActorSuspending(actor: ZLinkActor) {
                    val worker = DefaultZLinkIoWorkerCall(pool) { release }
                    // Signal only after Yield has relinquished the gate, if selected.
                    val waiting = if (yield) worker.yield() else worker.submit()
                    entered.complete(null)
                    waiting.await()
                }

                override suspend fun onLeaveActorSuspending(actor: ZLinkActor) {}
            }
        val lifecycle =
            serials.enqueueSpotLifecycleAdmission {
                ZLinkSuspendInvocationContext.enterApplicationExecution(
                        ZLinkSuspendInvocationContext.ApplicationExecution(
                            "room",
                            null,
                            true,
                            true,
                        ) {
                            false
                        }
                    )
                    .use {
                        val callback = spot.onJoinedActor(actor)
                        // Fix the prefix order: suspend the real coroutine before the runtime
                        // observes its returned stage. No observation interval decides overlap.
                        entered.get(completionSeconds, TimeUnit.SECONDS)
                        callback.whenComplete { _, failure ->
                            if (failure == null) callbackDone.complete(null)
                            else callbackDone.completeExceptionally(failure)
                        }
                    }
            }
        fun drain() {
            while (true) (tasks.poll() ?: return).run()
        }
        fun finish(stage: java.util.concurrent.CompletionStage<Void>) {
            val done = stage.toCompletableFuture()
            done.whenComplete { _, _ -> tasks.add(Runnable {}) }
            while (!done.isDone) {
                checkNotNull(tasks.poll(completionSeconds, TimeUnit.SECONDS)) {
                        "Runtime did not make progress"
                    }
                    .run()
            }
            done.join()
            drain()
        }
        try {
            drain()
            finish(entered)
            assertFalse(callbackDone.isDone, "joined callback must still await its worker")
            val timer =
                serials.executeTimer("probe") {
                    timerEntered.set(true)
                    CompletableFuture.completedFuture(null)
                }
            drain()
            val overlapped = timerEntered.get()
            release.complete(true)
            finish(lifecycle)
            finish(timer)
            assertTrue(callbackDone.isDone)
            assertTrue(timerEntered.get(), "timer must progress after joined completion")
            assertEquals(yield, overlapped, "timer entry during await requires explicit Yield")
        } finally {
            release.complete(true)
            finish(lifecycle)
            finish(serials.closeAsync())
            pool.close()
        }
    }
}
