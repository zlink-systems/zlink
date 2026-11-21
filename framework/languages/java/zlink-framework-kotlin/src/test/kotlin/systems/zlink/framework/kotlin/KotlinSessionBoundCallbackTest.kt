package systems.zlink.framework.kotlin

import java.lang.reflect.Modifier
import java.net.Socket
import java.net.URI
import java.util.concurrent.CompletableFuture
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.future.await
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertSame
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorCallbackCancellation
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.actors.ZLinkSessionBindingIdentity
import systems.zlink.framework.monitoring.ZLinkListenerKind
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.streams.ZLinkSessionContext

class KotlinSessionBoundCallbackTest {
    @Test
    fun defaultCallbackIsNoOpAndBridgeIsFinal() {
        val actor = DefaultActor(null)
        actor
            .onSessionBound(identity, ZLinkActorCallbackCancellation { false })
            .toCompletableFuture()
            .get(3, TimeUnit.SECONDS)
        val bridge =
            actor.javaClass.getMethod(
                "onSessionBound",
                ZLinkSessionBindingIdentity::class.java,
                ZLinkActorCallbackCancellation::class.java,
            )
        assertEquals(ZLinkSuspendingActor::class.java, bridge.declaringClass)
        assertTrue(Modifier.isFinal(bridge.modifiers))
    }

    @Test
    fun cancellationTokenIsPassedThroughAndStageCancellationCancelsCoroutine() {
        val requested = AtomicBoolean()
        val cancellation = ZLinkActorCallbackCancellation { requested.get() }
        val actor = BoundActor(null)
        actor.cancelOnly = true
        val stage = actor.onSessionBound(identity, cancellation).toCompletableFuture()
        assertEquals(identity, actor.entered.get(3, TimeUnit.SECONDS))
        assertSame(cancellation, actor.cancellation)
        assertFalse(actor.cancellation!!.isCancellationRequested)
        requested.set(true)
        assertTrue(actor.cancellation!!.isCancellationRequested)
        assertTrue(stage.cancel(true))
        actor.exited.get(3, TimeUnit.SECONDS)
        assertTrue(stage.isCancelled)
    }

    @Test
    fun suspendingCallbackRunsOnceAfterCommitAndBindDoesNotWaitForIt() {
        sessions.clear()
        val options = DefaultZLinkFrameworkOptions()
        options.addLocationStore(ZLinkInMemoryLocationStore())
        options
            .addRouteMesh("game")
            .listen("tcp://127.0.0.1:0")
            .objects()
            .server()
            .addEntrySpot(Entry::class.java)
            .addActorFactory("player", BoundActor::class.java, Factory::class.java) {
                it.disableRelocation()
            }
        options
            .addStreamNode("clients")
            .bind("tcp://127.0.0.1:0")
            .enableActorDispatch()
            .registerSession(Session::class.java)
        ZLinkFrameworkRuntimeTestAccess.startHost(options, ZLinkJavaBackendAdapterFactory()).use {
            runtime ->
            val created =
                runtime
                    .actorManager()
                    .create("bound", "player")
                    .submit()
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS) as ZLinkActorCreateResult.Created
            val actors = ZLinkFrameworkRuntimeTestAccess.actors(runtime)
            val actor = actors.localActor("bound").orElseThrow() as BoundActor
            actor.block = true
            actor.currentBinding = {
                actors.boundSessionRoute(actor).orElseThrow().sessionBindingIdentity()
            }
            val port =
                URI.create(runtime.listenerStatus(ZLinkListenerKind.STREAM, "clients").endpoint())
                    .port
            Socket("127.0.0.1", port).use {
                val session = sessions.poll(3, TimeUnit.SECONDS)!!
                try {
                    session
                        .actors()
                        .bind(created.actor())
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS)
                    val committed = actor.entered.get(3, TimeUnit.SECONDS)
                    assertEquals(committed, actor.observedBinding.get(3, TimeUnit.SECONDS))
                    assertEquals(1, actor.calls.get())
                    assertFalse(actor.exited.isDone)
                    // Looking up the same binding must not notify again.
                    session
                        .actors()
                        .bindOrGet(created.actor())
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS)
                    assertEquals(1, actor.calls.get())
                } finally {
                    actor.release.complete(null)
                    actor.exited.get(3, TimeUnit.SECONDS)
                }
            }
        }
    }

    open class DefaultActor(private val actorContext: ZLinkActorContext?) : ZLinkSuspendingActor() {
        override val context: ZLinkActorContext
            get() = actorContext!!

        override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {}
    }

    class BoundActor(context: ZLinkActorContext?) : DefaultActor(context) {
        val calls = AtomicInteger()
        val entered = CompletableFuture<ZLinkSessionBindingIdentity>()
        val release = CompletableFuture<Void>()
        val exited = CompletableFuture<Void>()
        var cancellation: ZLinkActorCallbackCancellation? = null
        val observedBinding = CompletableFuture<ZLinkSessionBindingIdentity>()
        var currentBinding: (() -> ZLinkSessionBindingIdentity)? = null
        var block = false
        var cancelOnly = false

        override suspend fun onSessionBoundSuspending(
            incoming: ZLinkSessionBindingIdentity,
            cancellation: ZLinkActorCallbackCancellation,
        ) {
            calls.incrementAndGet()
            this.cancellation = cancellation
            currentBinding?.let { observedBinding.complete(it()) }
            entered.complete(incoming)
            try {
                if (cancelOnly) awaitCancellation()
                if (block) release.await()
            } finally {
                exited.complete(null)
            }
        }
    }

    class Factory : ZLinkSuspendingActorFactory() {
        override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
            BoundActor(context)
    }

    class Entry(override val context: ZLinkEntrySpotContext) :
        ZLinkSuspendingEntrySpot<BoundActor>() {
        override suspend fun onJoinedActorSuspending(actor: BoundActor) {}

        override suspend fun onLeaveActorSuspending(actor: BoundActor) {}

        override suspend fun onDisconnectActorSuspending(actor: BoundActor) {}
    }

    class Session(private val session: ZLinkSessionContext) : ZLinkSuspendingSession() {
        override fun context(): ZLinkSessionContext = session

        override suspend fun onConnectedSuspending() {
            sessions.add(session)
        }
    }

    companion object {
        private val identity = ZLinkSessionBindingIdentity(RoutingId.from("owner"), 1, 1)
        private val sessions = LinkedBlockingQueue<ZLinkSessionContext>()
    }
}
