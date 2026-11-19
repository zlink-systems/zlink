package systems.zlink.framework.kotlin

import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import java.util.logging.FileHandler
import java.util.logging.Logger
import java.util.logging.SimpleFormatter
import kotlinx.coroutines.future.await
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertNotEquals
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.Timeout
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorFactory
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.monitoring.ZLinkListenerKind
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore
import systems.zlink.framework.spots.ZLinkEntrySpot
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle
import systems.zlink.framework.testsupport.ZLinkHostServing

class KotlinRecreatedActorRoutingTest {
    @Test
    // Bounds host startup and Serving, which run outside the scenario deadline.
    @Timeout(value = 60, threadMode = Timeout.ThreadMode.SEPARATE_THREAD)
    fun cachedIdSendAndRequestReachTheRecreatedActor() = runBlocking {
        val logger = Logger.getLogger(ZLinkMessageFlowTracer::class.java.name)
        val file =
            FileHandler(System.getenv("ZLINK_KOTLIN_FLOW_FILE") ?: "build/kotlin-recreated.flow")
        file.formatter = SimpleFormatter()
        logger.addHandler(file)
        try {
            received = CompletableFuture()
            val locations = ZLinkInMemoryLocationStore()
            val owner = lifecycle(locations, null)
            owner.start()
            try {
                assertTrue(
                    ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(owner.runtimeBean()).await()
                )
                ZLinkFrameworkRuntimeTestAccess.startupCompletion(owner.runtimeBean()).await()
                ZLinkHostServing.await(owner.runtimeBean())
                val previous =
                    (owner.actorManager().kotlin().create(ACTOR, "player").inMesh(MESH).await()
                            as ZLinkActorCreateResult.Created)
                        .actor()
                val endpoint =
                    owner
                        .runtimeBean()
                        .listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH)
                        .endpoint()
                val caller = lifecycle(locations, endpoint)
                caller.start()
                try {
                    assertTrue(
                        ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(caller.runtimeBean())
                            .await()
                    )
                    ZLinkFrameworkRuntimeTestAccess.startupCompletion(caller.runtimeBean()).await()
                    ZLinkHostServing.await(caller.runtimeBean())
                    val client = caller.actorClient().kotlin()
                    assertEquals(
                        previous.objectGeneration().toString(),
                        client.requestToActor(ACTOR, Probe(), String::class).await(),
                    )
                    withTimeout(20000) {
                        assertTrue(owner.actorManager().kotlin().destroy(previous))
                        val current =
                            (owner
                                    .actorManager()
                                    .kotlin()
                                    .create(ACTOR, "player")
                                    .inMesh(MESH)
                                    .await() as ZLinkActorCreateResult.Created)
                                .actor()
                        assertEquals(previous.nodeRid(), current.nodeRid())
                        assertNotEquals(previous.objectGeneration(), current.objectGeneration())
                        client.sendToActor(ACTOR, Notice()).await()
                        assertEquals(current.objectGeneration(), received.await())
                        assertEquals(
                            current.objectGeneration().toString(),
                            client.requestToActor(ACTOR, Probe(), String::class).await(),
                        )
                    }
                } finally {
                    caller.stop()
                }
            } finally {
                owner.stop()
            }
        } finally {
            logger.removeHandler(file)
            file.close()
        }
    }

    private fun lifecycle(
        locations: ZLinkInMemoryLocationStore,
        endpoint: String?,
    ): ZLinkFrameworkLifecycle {
        val options = DefaultZLinkFrameworkOptions()
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL)
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1))
        options.addLocationStore(locations)
        val mesh = options.addRouteMesh(MESH)
        mesh.listen("tcp://127.0.0.1:0")
        if (endpoint == null) {
            mesh.objects().server().addEntrySpot(Entry::class.java)
            mesh.objects().server().addActorFactory(
                "player",
                Player::class.java,
                Factory::class.java,
            ) {
                it.disableRelocation()
            }
        } else {
            mesh.peerConnections().connect(endpoint)
            mesh.objects().client()
        }
        return ZLinkFrameworkLifecycle(
            options,
            ZLinkJavaBackendAdapterFactory(),
            ZLinkHandlerActivator.reflection(),
        )
    }

    data class Probe(val value: Int = 0)

    data class Notice(val value: Int = 0)

    class Player(private val actorContext: ZLinkActorContext) : ZLinkActor {
        override fun context() = actorContext
    }

    class Factory : ZLinkActorFactory {
        override fun create(context: ZLinkActorContext): CompletionStage<ZLinkActor> =
            CompletableFuture.completedFuture(Player(context))
    }

    class Entry(private val entryContext: ZLinkEntrySpotContext) : ZLinkEntrySpot<Player> {
        override fun context() = entryContext

        override fun configure() {
            entryContext.handlers().addHandler(ProbeHandler::class.java)
            entryContext.handlers().addHandler(NoticeHandler::class.java)
        }

        override fun onJoinedActor(actor: Player) = CompletableFuture.completedFuture<Void>(null)

        override fun onLeaveActor(actor: Player) = CompletableFuture.completedFuture<Void>(null)
    }

    class ProbeHandler : ZLinkEntrySpotActorRequestHandler<Entry, Player, Probe, String> {
        override fun handle(
            spot: Entry,
            actor: Player,
            context: ZLinkMessageContext,
            request: Probe,
        ): CompletionStage<String> =
            CompletableFuture.completedFuture(actor.context().objectGeneration().toString())
    }

    class NoticeHandler : ZLinkEntrySpotActorSendHandler<Entry, Player, Notice> {
        override fun handle(
            spot: Entry,
            actor: Player,
            context: ZLinkMessageContext,
            message: Notice,
        ): CompletionStage<Void> {
            received.complete(actor.context().objectGeneration())
            return CompletableFuture.completedFuture(null)
        }
    }

    companion object {
        private const val MESH = "kotlin-recreated-actor"
        private const val ACTOR = "retired-player"
        private var received = CompletableFuture<Long>()
    }
}
