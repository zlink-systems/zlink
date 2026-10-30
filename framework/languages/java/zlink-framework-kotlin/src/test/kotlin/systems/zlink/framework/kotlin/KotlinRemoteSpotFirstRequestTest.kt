package systems.zlink.framework.kotlin

import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore
import systems.zlink.framework.spots.ZLinkSpot
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotCreateState
import systems.zlink.framework.spots.ZLinkSpotRequestHandler
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle

class KotlinRemoteSpotFirstRequestTest {
    @Test
    fun freshObjectClientAwaitsItsFirstExistingRemoteSpotRequestOnce() = runBlocking {
        withTimeout(15000) {
            calls.set(0)
            val locations = ZLinkInMemoryLocationStore()
            val owner = lifecycle(locations, "kotlin-owner", true)
            try {
                owner.start()
                val created =
                    owner.spotManager().kotlin().getOrCreate(SPOT, TYPE).inMesh(MESH).await()
                val requester = lifecycle(locations, "kotlin-requester", false)
                try {
                    requester.start()
                    val existing =
                        requester
                            .spotManager()
                            .kotlin()
                            .getOrCreate(SPOT, TYPE)
                            .inMesh(MESH)
                            .await()
                    assertEquals(ZLinkSpotCreateState.EXISTING, existing.state())
                    assertEquals(created.spot(), existing.spot())
                    val reply = runCatching {
                        (requester as ZLinkRouteClient)
                            .kotlin()
                            .requestToSpot(SPOT, Probe("first"), String::class)
                            .inMesh(MESH)
                            .timeout(Duration.ofSeconds(5))
                            .await()
                    }
                    assertTrue(
                        reply.isSuccess,
                        "first remote request failed: ${reply.exceptionOrNull()}",
                    )
                    assertEquals("first", reply.getOrThrow())
                    assertEquals(1, calls.get())
                } finally {
                    requester.stop()
                }
            } finally {
                owner.stop()
            }
        }
    }

    private fun lifecycle(
        locations: ZLinkInMemoryLocationStore,
        rid: String,
        server: Boolean,
    ): ZLinkFrameworkLifecycle {
        val options = DefaultZLinkFrameworkOptions()
        options.addLocationStore(locations)
        val mesh =
            options.addRouteMesh(MESH).setRoutingId(RoutingId.from(rid)).listen("tcp://127.0.0.1:0")
        if (server)
            mesh.objects().server().addSpotFactory(TYPE, Echo::class.java) {
                it.disableRelocation()
            }
        else mesh.objects().client()
        return ZLinkFrameworkLifecycle(
            options,
            ZLinkJavaBackendAdapterFactory(),
            ZLinkHandlerActivator.reflection(),
        )
    }

    data class Probe(val value: String)

    class Echo(private val spotContext: ZLinkSpotContext) : ZLinkSpot<ZLinkActor> {
        override fun context(): ZLinkSpotContext = spotContext

        override fun onJoinedActor(actor: ZLinkActor) =
            CompletableFuture.completedFuture<Void>(null)

        override fun onLeaveActor(actor: ZLinkActor) = CompletableFuture.completedFuture<Void>(null)

        override fun configure() {
            spotContext.handlers().addHandler(EchoHandler::class.java)
        }
    }

    class EchoHandler : ZLinkSpotRequestHandler<Echo, Probe, String> {
        override fun handle(spot: Echo, request: Probe): CompletionStage<String> {
            calls.incrementAndGet()
            return CompletableFuture.completedFuture(request.value)
        }
    }

    companion object {
        private const val MESH = "kotlin-first-request"
        private const val SPOT = "kotlin-first-request-spot"
        private const val TYPE = "kotlin-echo"
        private val calls = AtomicInteger()
    }
}
