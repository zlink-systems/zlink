package systems.zlink.framework.kotlin

import java.lang.reflect.Proxy
import java.time.Duration
import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.runBlocking
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertSame
import org.junit.jupiter.api.assertThrows
import org.junit.jupiter.params.ParameterizedTest
import org.junit.jupiter.params.provider.ValueSource
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer

class KotlinActorCreationCallOwnershipTest {
    @ParameterizedTest
    @ValueSource(booleans = [false, true])
    fun duplicateOptionsPreserveJavaErrorAndOriginalValues(getOrCreate: Boolean) = runBlocking {
        val fixture = Fixture()
        val call = fixture.call(getOrCreate)
            .inMesh("mesh-a")
            .request("first")
            .timeout(Duration.ofSeconds(2))

        assertInvalidOperation { call.inMesh("other-mesh") }
        assertInvalidOperation { call.request("second") }
        assertInvalidOperation { call.timeout(Duration.ofSeconds(3)) }
        assertEquals(0, fixture.submissions)

        fixture.completion.complete(fixture.result)
        assertSame(fixture.result, call.await())
        assertEquals(1, fixture.submissions)
        assertEquals("first", fixture.request?.decode(String::class.java))
        assertEquals(Duration.ofSeconds(2), fixture.timeout)
        assertEquals(getOrCreate, fixture.getOrCreate)
    }

    @ParameterizedTest
    @ValueSource(booleans = [false, true])
    fun yieldOutsideOwnerTurnUsesInvalidOperation(getOrCreate: Boolean) = runBlocking {
        val fixture = Fixture()
        val call = fixture.call(getOrCreate)

        assertInvalidOperation { call.yield() }
        assertEquals(0, fixture.submissions)
    }

    @ParameterizedTest
    @ValueSource(booleans = [false, true])
    fun yieldOutsideOwnerTurnLeavesCallAvailable(getOrCreate: Boolean) = runBlocking {
        val fixture = Fixture()
        val call = fixture.call(getOrCreate)

        repeat(2) {
            assertThrows<ZLinkFrameworkException> { call.yield() }
            assertEquals(0, fixture.submissions)
        }

        fixture.completion.complete(fixture.result)
        assertSame(fixture.result, call.await())
        assertEquals(1, fixture.submissions)
        assertEquals(getOrCreate, fixture.getOrCreate)
        assertInvalidOperation { call.await() }
        assertEquals(1, fixture.submissions)
    }

    @ParameterizedTest
    @ValueSource(booleans = [false, true])
    fun resubmitWhileSuspendedAndAfterCompletionUsesJavaOwner(getOrCreate: Boolean) = runBlocking {
        val fixture = Fixture()
        val call = fixture.call(getOrCreate)
        val first = async(start = CoroutineStart.UNDISPATCHED) { call.await() }

        assertFalse(first.isCompleted)
        assertInvalidOperation { call.await() }
        assertEquals(1, fixture.submissions)

        fixture.completion.complete(fixture.result)
        assertSame(fixture.result, first.await())
        assertInvalidOperation { call.await() }
        assertEquals(1, fixture.submissions)
        assertEquals(getOrCreate, fixture.getOrCreate)
    }

    private inline fun assertInvalidOperation(operation: () -> Unit) {
        assertEquals(
            ZLinkFrameworkErrorKind.INVALID_OPERATION,
            assertThrows<ZLinkFrameworkException> { operation() }.kind(),
        )
    }

    private class Fixture {
        val completion = CompletableFuture<ZLinkActorCreateResult>()
        val result = ZLinkActorCreateResult.Created(
            ActorRef("actor-a", 1, "mesh-a", RoutingId.from("node-a")),
            ZLinkMessage.empty(),
        )
        var submissions = 0
        var request: ZLinkMessage? = null
        var timeout: Duration? = null
        var getOrCreate = false
        private val runtime: ZLinkActorRuntime

        init {
            val node = Proxy.newProxyInstance(
                ZLinkInternalSpotNode::class.java.classLoader,
                arrayOf(ZLinkInternalSpotNode::class.java),
            ) { _, method, _ ->
                if (method.name == "routingId") {
                    RoutingId.from("node-a")
                } else {
                    throw AssertionError("unexpected backend call: $method")
                }
            } as ZLinkInternalSpotNode
            runtime = ZLinkActorRuntime(
                node, emptyMap(), Duration.ofSeconds(5), ZLinkJsonMessageSerializer(),
            )
            runtime.setMeshName("mesh-a")
            runtime.setCreationSubmitter { id, type, message, get, deadline ->
                assertEquals("actor-a", id)
                assertEquals("player", type)
                submissions++
                request = message
                getOrCreate = get
                timeout = deadline
                completion
            }
        }

        fun call(getOrCreate: Boolean): ZLinkKotlinActorCreateCall =
            if (getOrCreate) {
                runtime.kotlin().getOrCreate("actor-a", "player")
            } else {
                runtime.kotlin().create("actor-a", "player")
            }
    }
}
