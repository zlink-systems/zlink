package systems.zlink.framework.runtime.spots

import java.time.Duration
import java.util.ArrayDeque
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import java.util.concurrent.atomic.AtomicInteger
import kotlin.coroutines.CoroutineContext
import kotlinx.coroutines.CoroutineDispatcher
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertInstanceOf
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue
import systems.zlink.framework.execution.ZLinkWorkerPool
import systems.zlink.framework.kotlin.ProfileReply
import systems.zlink.framework.kotlin.ProfileRequest
import systems.zlink.framework.kotlin.ZLinkCoroutineSuspendHandlerInvoker
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext

class KotlinYieldAdmissionTest {
    @Test
    fun secondYieldRejectsBeforeSubmissionInSuspendHandler() {
        val handler = YieldAdmissionHandler(false)
        runHandler(handler) { serial, dispatcher, _ ->
            serial.drain()
            dispatcher.drain()
            handler.first.complete(1)
        }
        assertEquals(1, handler.submissions.get(), "submissions in original execution turn")
        val error = assertInstanceOf(ZLinkFrameworkException::class.java, handler.secondFailure)
        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION, error.kind())
    }

    @Test
    fun resumedNewTurnCanYieldAgainAfterDispatcherWasHeld() {
        val handler = YieldAdmissionHandler(true)
        runHandler(handler) { serial, dispatcher, queue ->
            serial.drain()
            dispatcher.drain()
            assertEquals(1, handler.submissions.get())
            handler.first.complete(1)
            serial.drain()
            // The coroutine resume is queued while its dispatcher remains held.
            assertEquals(1, handler.submissions.get())
            val siblingStarts = AtomicInteger()
            val sibling =
                queue.enqueue(
                    {
                        siblingStarts.incrementAndGet()
                        CompletableFuture.completedFuture(null)
                    },
                    null,
                )
            serial.drain()
            assertEquals(
                0,
                siblingStarts.get(),
                "resumed turn owns the gate while dispatcher is held",
            )
            dispatcher.drain()
            assertEquals(2, handler.submissions.get(), "new execution turn must accept Yield")
            serial.drain()
            assertEquals(1, siblingStarts.get(), "next Yield releases gate to sibling")
            assertTrue(sibling.toCompletableFuture().isDone())
        }
        assertEquals(null, handler.secondFailure)
    }

    private fun runHandler(
        handler: YieldAdmissionHandler,
        exercise: (ManualTasks, ManualDispatcher, ZLinkSerialExecutionQueue) -> Unit,
    ) {
        val serial = ManualTasks()
        val dispatcher = ManualDispatcher()
        val queue = ZLinkSerialExecutionQueue(serial, ZLinkExecutionLanePolicy.spot())
        val method = handler.javaClass.methods.single { it.name == "request" }
        val invocation =
            ZLinkHandlerMethodInvoker.prepare(
                method,
                listOf(ZLinkCoroutineSuspendHandlerInvoker(dispatcher)),
                0,
            )
        val result = CompletableFuture<Any?>()
        val turn =
            queue.enqueue(
                {
                    ZLinkSuspendInvocationContext.enterApplicationExecution(
                            ZLinkSuspendInvocationContext.ApplicationExecution(
                                "room-1",
                                null,
                                true,
                                true,
                            ) {
                                false
                            }
                        )
                        .use {
                            invocation
                                .invoke(handler, ProfileRequest("Ada"))
                                .whenComplete { reply, error ->
                                    if (error == null) result.complete(reply)
                                    else result.completeExceptionally(error)
                                }
                                .thenApply { null }
                        }
                },
                null,
            )
        try {
            exercise(serial, dispatcher, queue)
        } finally {
            handler.first.complete(1)
            handler.second.complete(2)
            do {
                serial.drain()
                dispatcher.drain()
            } while (serial.pending() || dispatcher.tasks.pending())
            handler.pool.close()
            queue.close()
        }
        assertTrue(turn.toCompletableFuture().isDone(), "handler and resumed turns must settle")
        turn.toCompletableFuture().join()
        assertEquals(ProfileReply(if (handler.sequential) "2" else "1"), result.join())
    }
}

private class ManualTasks : java.util.concurrent.AbstractExecutorService() {
    override fun shutdown() {}

    override fun shutdownNow(): MutableList<Runnable> = mutableListOf()

    override fun isShutdown() = false

    override fun isTerminated() = false

    override fun awaitTermination(value: Long, unit: java.util.concurrent.TimeUnit) = true

    private val tasks = ArrayDeque<Runnable>()

    override fun execute(task: Runnable) {
        tasks.add(task)
    }

    fun drain() {
        while (tasks.isNotEmpty()) tasks.remove().run()
    }

    fun pending() = tasks.isNotEmpty()
}

private class ManualDispatcher : CoroutineDispatcher() {
    val tasks = ManualTasks()

    override fun dispatch(context: CoroutineContext, block: Runnable) = tasks.execute(block)

    fun drain() = tasks.drain()
}

class YieldAdmissionHandler(val sequential: Boolean) {
    val pool = ZLinkWorkerPool(0, 1, Duration.ofSeconds(30))
    val first = CompletableFuture<Int>()
    val second = CompletableFuture<Int>()
    val submissions = AtomicInteger()
    var secondFailure: ZLinkFrameworkException? = null

    suspend fun request(request: ProfileRequest): ProfileReply {
        val initial = submitYield(first)
        if (sequential) {
            initial.await()
            return ProfileReply(submitYield(second).await().toString())
        }
        try {
            submitYield(second)
        } catch (error: ZLinkFrameworkException) {
            secondFailure = error
        }
        return ProfileReply(initial.await().toString())
    }

    private fun submitYield(remote: CompletionStage<Int>): CompletionStage<Int> {
        return DefaultZLinkIoWorkerCall(pool) { _ ->
                submissions.incrementAndGet()
                remote
            }
            .yield()
    }
}
