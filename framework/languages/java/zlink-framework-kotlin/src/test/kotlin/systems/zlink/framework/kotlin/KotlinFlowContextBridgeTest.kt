package systems.zlink.framework.kotlin

import java.util.Optional
import java.util.concurrent.CompletableFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.future.await
import kotlinx.coroutines.launch
import kotlinx.coroutines.yield
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertNotNull
import org.junit.jupiter.api.Assertions.assertNull
import org.junit.jupiter.api.Assertions.assertSame
import org.junit.jupiter.api.Test
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorJoinCall
import systems.zlink.framework.actors.ZLinkBoundSession
import systems.zlink.framework.monitoring.ZLinkFlowOrigin
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext

class KotlinFlowContextBridgeTest {
    @Test
    fun `all lane barrier waits for yielded coroutine continuation`() {
        val queue = ZLinkAsyncSerialQueue()
        val remote = CompletableFuture<Void>()
        val yielded = CompletableFuture<Void>()

        val dispatch = queue.enqueue {
            val completed = CompletableFuture<Void>()
            CoroutineScope(
                ZLinkCoroutineInvocationContext.capture(Dispatchers.Default),
            ).launch {
                yielded.complete(null)
                ZLinkAsyncSerialQueue.yieldCurrent(remote).await()
                completed.complete(null)
            }
            completed
        }.toCompletableFuture()

        yielded.get(3, TimeUnit.SECONDS)
        val barrier = queue.awaitQuiescence().toCompletableFuture()
        assertFalse(barrier.isDone)

        remote.complete(null)
        dispatch.get(3, TimeUnit.SECONDS)
        barrier.get(3, TimeUnit.SECONDS)
    }

    @Test
    fun `suspending handler preserves application execution and serial claim`() {
        val queue = ZLinkAsyncSerialQueue()
        val beforeTurn = AtomicReference<Any>()
        val afterTurn = AtomicReference<Any>()
        val firstRemote = CompletableFuture<Void>()
        val secondRemote = CompletableFuture<Void>()
        val firstYieldStarted = CompletableFuture<Void>()
        val secondYieldStarted = CompletableFuture<Void>()
        val execution = ZLinkSuspendInvocationContext.ApplicationExecution(
            "room-1",
            "actor-a",
            true,
            true,
        ) { false }

        val first = queue.enqueue {
            ZLinkSuspendInvocationContext.enterApplicationExecution(execution).use {
                beforeTurn.set(ZLinkSuspendInvocationContext.currentSerialExecutionTurn())
                val completed = CompletableFuture<Void>()
                CoroutineScope(
                    ZLinkCoroutineInvocationContext.capture(Dispatchers.Default),
                ).launch {
                    firstYieldStarted.complete(null)
                    ZLinkAsyncSerialQueue.yieldCurrent(firstRemote).await()
                    assertSame(
                        execution,
                        ZLinkSuspendInvocationContext.currentApplicationExecution(),
                    )
                    afterTurn.set(
                        ZLinkSuspendInvocationContext.currentSerialExecutionTurn(),
                    )
                    secondYieldStarted.complete(null)
                    ZLinkAsyncSerialQueue.yieldCurrent(secondRemote).await()
                    completed.complete(null)
                }
                completed
            }
        }.toCompletableFuture()

        firstYieldStarted.get(3, TimeUnit.SECONDS)
        queue.enqueue { CompletableFuture.completedFuture(null) }
            .toCompletableFuture().get(3, TimeUnit.SECONDS)
        firstRemote.complete(null)
        secondYieldStarted.get(3, TimeUnit.SECONDS)
        queue.enqueue { CompletableFuture.completedFuture(null) }
            .toCompletableFuture().get(3, TimeUnit.SECONDS)
        secondRemote.complete(null)
        first.get(3, TimeUnit.SECONDS)

        assertNotNull(beforeTurn.get())
        assertSame(beforeTurn.get(), afterTurn.get())
    }

    @Test
    fun `suspending lifecycle preserves flow across suspension and clears it afterward`() {
        val flow = ZLinkFlowContext.create(ZLinkFlowOrigin.LIFECYCLE)
        val first = RecordingActorFactory()
        val firstStage = ZLinkFlowContext.enter(flow).use {
            first.create(context("actor-a"))
        }

        firstStage.toCompletableFuture().join()

        assertEquals(flow, first.beforeSuspension)
        assertEquals(flow, first.afterSuspension)

        val second = RecordingActorFactory()
        second.create(context("actor-b")).toCompletableFuture().join()
        assertNull(second.beforeSuspension)
        assertNull(second.afterSuspension)
    }

    private class RecordingActorFactory : ZLinkSuspendingActorFactory() {
        var beforeSuspension: ZLinkFlowContext.State? = null
        var afterSuspension: ZLinkFlowContext.State? = null

        override suspend fun createActor(context: ZLinkActorContext): ZLinkActor {
            beforeSuspension = ZLinkFlowContext.current()
            yield()
            afterSuspension = ZLinkFlowContext.current()
            return object : ZLinkActor {
                override fun context() = context
            }
        }
    }

    companion object {
        private fun context(actorId: String) = object : ZLinkActorContext {
            override fun actorId(): String = actorId
            override fun objectGeneration(): Long = 1L
            override fun meshName(): String = "test"
            override fun spotId(): Optional<String> = Optional.empty()
            override fun boundSession(): ZLinkBoundSession = error("not used")
            override fun joinSpot(spotId: String): ZLinkActorJoinCall =
                error("not used")
            override fun joinSpot(spotId: String, request: Any): ZLinkActorJoinCall =
                error("not used")
            override fun joinEntrySpot(): ZLinkActorJoinCall = error("not used")
            override fun joinEntrySpot(request: Any): ZLinkActorJoinCall = error("not used")
        }
    }
}
