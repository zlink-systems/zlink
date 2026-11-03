package systems.zlink.framework.kotlin

import java.io.EOFException
import java.io.InputStream
import java.net.Socket
import java.net.URI
import java.nio.ByteBuffer
import java.nio.charset.StandardCharsets
import java.util.EnumSet
import java.util.Optional
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.future.await
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import systems.zlink.contracts.core.RoutingId
import systems.zlink.contracts.core.Zlink
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorCallbackCancellation
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.actors.ZLinkSessionBindingIdentity
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.monitoring.ZLinkListenerKind
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkStreamCodec
import systems.zlink.framework.streams.ZLinkStreamMessageKind

class KotlinDuplicateBindCallbackPresenceTest {
    @Test
    fun javaReflectionSeesTheFinalBridgeWhileTheSuspendCallbackRemainsInherited() {
        val actorType = DefaultSuspendingActor::class.java
        val bridge =
            actorType.getMethod(
                "onDuplicateBind",
                systems.zlink.framework.actors.ZLinkSessionBindingIdentity::class.java,
                systems.zlink.framework.actors.ZLinkSessionBindingIdentity::class.java,
                systems.zlink.framework.actors.ZLinkActorCallbackCancellation::class.java,
            )
        val suspendingCallback =
            actorType.getMethod(
                "onDuplicateBindSuspending",
                systems.zlink.framework.actors.ZLinkSessionBindingIdentity::class.java,
                systems.zlink.framework.actors.ZLinkSessionBindingIdentity::class.java,
                systems.zlink.framework.actors.ZLinkActorCallbackCancellation::class.java,
                kotlin.coroutines.Continuation::class.java,
            )

        assertEquals(ZLinkSuspendingActor::class.java, bridge.declaringClass)
        assertEquals(ZLinkSuspendingActor::class.java, suspendingCallback.declaringClass)
        assertTrue(bridge.declaringClass != ZLinkActor::class.java)
    }

    @Test
    fun javaReflectionFindsAnOverriddenSuspendingCallbackOnTheConcreteActor() {
        val actorType = OverridingSuspendingActor::class.java
        val bridge =
            actorType.getMethod(
                "onDuplicateBind",
                ZLinkSessionBindingIdentity::class.java,
                ZLinkSessionBindingIdentity::class.java,
                ZLinkActorCallbackCancellation::class.java,
            )
        val suspendingCallback =
            actorType.getMethod(
                "onDuplicateBindSuspending",
                ZLinkSessionBindingIdentity::class.java,
                ZLinkSessionBindingIdentity::class.java,
                ZLinkActorCallbackCancellation::class.java,
                kotlin.coroutines.Continuation::class.java,
            )

        assertEquals(ZLinkSuspendingActor::class.java, bridge.declaringClass)
        assertEquals(OverridingSuspendingActor::class.java, suspendingCallback.declaringClass)
    }

    @Test
    fun replacementBindDoesNotWaitForPreviousSessionDisconnectTurn() {
        Zlink.version()
        val runId = UUID.randomUUID().toString()
        val locations = ZLinkInMemoryLocationStore()
        val actorOptions = DefaultZLinkFrameworkOptions()
        actorOptions.addLocationStore(locations)
        val actorMesh = actorOptions.addRouteMesh(MESH)
        actorMesh.listen("tcp://127.0.0.1:0").setRoutingId(RoutingId.from("owner-$runId"))
        actorMesh.objects().server().addEntrySpot(DisconnectEntry::class.java).addActorFactory(
            ACTOR_TYPE,
            DefaultSuspendingActor::class.java,
            DefaultSuspendingActorFactory::class.java,
        ) { factory ->
            factory.disableRelocation()
        }
        val sessionOptions = DefaultZLinkFrameworkOptions()
        sessionOptions.addLocationStore(locations)
        val sessionMesh = sessionOptions.addRouteMesh(MESH)
        sessionMesh.listen("tcp://127.0.0.1:0").setRoutingId(RoutingId.from("session-$runId"))
        sessionMesh.objects().client()
        sessionOptions
            .addStreamNode(STREAM_NODE)
            .bind("tcp://127.0.0.1:0")
            .enableActorDispatch()
            .registerSession(BindSession::class.java)

        val actorRuntime =
            ZLinkFrameworkRuntimeTestAccess.startHost(
                actorOptions,
                ZLinkJavaBackendAdapterFactory(),
            )
        val sessionRuntime =
            ZLinkFrameworkRuntimeTestAccess.startHost(
                sessionOptions,
                ZLinkJavaBackendAdapterFactory(),
            )
        try {
            val port =
                URI.create(
                        sessionRuntime
                            .listenerStatus(ZLinkListenerKind.STREAM, STREAM_NODE)
                            .endpoint()
                    )
                    .port
            repeat(REPLACEMENT_ATTEMPTS) { attempt ->
                val actorId = "player-$runId-$attempt"
                val gate = DisconnectGate()
                disconnectGates[actorId] = gate

                try {
                    Socket("127.0.0.1", port).use { previous ->
                        previous.soTimeout = CLIENT_TIMEOUT_MILLIS
                        sendRequest(previous, 1, BIND_PACKET, json(actorId))
                        assertBoundControl(previous, actorId)
                        assertReply(previous, 1, "\"bound\"")

                        previous.close()
                        gate.entered.get(CLIENT_TIMEOUT_MILLIS.toLong(), TimeUnit.MILLISECONDS)
                        Socket("127.0.0.1", port).use { incoming ->
                            incoming.soTimeout = CLIENT_TIMEOUT_MILLIS
                            sendRequest(incoming, 1, BIND_PACKET, json(actorId))
                            assertBoundControl(incoming, actorId)
                            assertReply(incoming, 1, "\"bound\"")
                            assertFalse(gate.release.isDone)
                        }
                    }
                } finally {
                    gate.release.complete(null)
                    gate.completed.get(CLIENT_TIMEOUT_MILLIS.toLong(), TimeUnit.MILLISECONDS)
                    disconnectGates.remove(actorId, gate)
                }
            }
        } finally {
            disconnectGates.values.forEach { it.release.complete(null) }
            sessionRuntime.close()
            actorRuntime.close()
            disconnectGates.clear()
        }
    }

    class DefaultSuspendingActor(override val context: ZLinkActorContext) : ZLinkSuspendingActor() {
        override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {}
    }

    class OverridingSuspendingActor(override val context: ZLinkActorContext) :
        ZLinkSuspendingActor() {
        override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {}

        override suspend fun onDuplicateBindSuspending(
            current: ZLinkSessionBindingIdentity,
            incoming: ZLinkSessionBindingIdentity,
            cancellation: ZLinkActorCallbackCancellation,
        ): String? = "reject"
    }

    class DefaultSuspendingActorFactory : ZLinkSuspendingActorFactory() {
        override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
            DefaultSuspendingActor(context)
    }

    class DisconnectEntry(override val context: ZLinkEntrySpotContext) :
        ZLinkSuspendingEntrySpot<DefaultSuspendingActor>() {
        override suspend fun onJoinedActorSuspending(actor: DefaultSuspendingActor) {}

        override suspend fun onLeaveActorSuspending(actor: DefaultSuspendingActor) {}

        override suspend fun onDisconnectActorSuspending(actor: DefaultSuspendingActor) {
            val gate = disconnectGates[actor.context.actorId()] ?: return
            gate.entered.complete(null)
            try {
                gate.release.await()
            } finally {
                gate.completed.complete(null)
            }
        }
    }

    class BindSession(
        private val session: ZLinkSessionContext,
        private val actors: ZLinkActorManager,
    ) : ZLinkSuspendingSession() {
        override fun context(): ZLinkSessionContext = session

        override suspend fun onDispatchSuspending(
            dispatch: ZLinkSessionDispatchContext,
            payload: ZLinkMessage,
        ) {
            check(dispatch.packetName() == BIND_PACKET)
            val actorId = payload.decode(String::class.java)
            val actor =
                when (
                    val result =
                        actors
                            .kotlin()
                            .getOrCreate(actorId, ACTOR_TYPE)
                            .inMesh(MESH)
                            .request(ZLinkMessage.empty())
                            .await()
                ) {
                    is ZLinkActorCreateResult.Created -> result.actor()
                    is ZLinkActorCreateResult.Existing -> result.actor()
                    is ZLinkActorCreateResult.Rejected -> error("Actor creation rejected: $result")
                }
            session.actors().bindOrGet(actor).await()
            session.client().reply("bound").submit().await()
        }
    }

    private data class DisconnectGate(
        val entered: CompletableFuture<Void> = CompletableFuture(),
        val release: CompletableFuture<Void> = CompletableFuture(),
        val completed: CompletableFuture<Void> = CompletableFuture(),
    )

    private data class InboundFrame(val header: ZLinkStreamHeader, val body: ByteArray)

    private fun sendRequest(socket: Socket, sequence: Long, packetName: String, body: ByteArray) {
        val header =
            ZLinkStreamHeader(
                ZLinkStreamMessageKind.REQUEST,
                ZLinkStreamCodec.RAW,
                EnumSet.noneOf(ZLinkStreamHeaderFlag::class.java),
                Optional.of(sequence),
                packetName,
                emptyMap(),
                Optional.empty(),
            )
        socket.getOutputStream().write(frame(ZLinkStreamHeaderCodec.encode(header), body))
        socket.getOutputStream().flush()
    }

    private fun assertBoundControl(socket: Socket, actorId: String) {
        while (true) {
            val frame = readFrame(socket)
            val header = frame.header
            if (
                header.kind() == ZLinkStreamMessageKind.CONTROL &&
                    header.name() == HEARTBEAT_PING_PACKET
            ) {
                val pong =
                    ZLinkStreamHeader(
                        ZLinkStreamMessageKind.CONTROL,
                        ZLinkStreamCodec.RAW,
                        EnumSet.noneOf(ZLinkStreamHeaderFlag::class.java),
                        Optional.empty(),
                        HEARTBEAT_PONG_PACKET,
                        emptyMap(),
                    )
                socket
                    .getOutputStream()
                    .write(frame(ZLinkStreamHeaderCodec.encode(pong), byteArrayOf()))
                socket.getOutputStream().flush()
                continue
            }
            assertEquals(
                ZLinkStreamMessageKind.CONTROL,
                header.kind(),
                "unexpected stream frame header=$header body=${String(frame.body, StandardCharsets.UTF_8)}",
            )
            assertEquals("\$zlink.actor.bound", header.name())
            val payload = ByteBuffer.wrap(frame.body)
            assertEquals(1, payload.get().unsigned())
            assertTrue(payload.short.unsigned() > 0)
            val actorLength = payload.get().unsigned()
            assertEquals(actorLength, payload.remaining())
            assertEquals(actorId, String(frame.body, 4, actorLength, StandardCharsets.UTF_8))
            return
        }
    }

    private fun assertReply(socket: Socket, sequence: Long, expectedBody: String) {
        val frame = readFrame(socket)
        assertEquals(ZLinkStreamMessageKind.RESPONSE, frame.header.kind())
        assertEquals(sequence, frame.header.requestSequence().orElseThrow())
        assertEquals(expectedBody, String(frame.body, StandardCharsets.UTF_8))
    }

    private fun readFrame(socket: Socket): InboundFrame {
        val input = socket.getInputStream()
        val prefix = readExactly(input, 6)
        val sizes = ByteBuffer.wrap(prefix)
        val headerSize = sizes.short.unsigned()
        val bodySize = sizes.int
        val header = ZLinkStreamHeaderCodec.decodeOrPlain(readExactly(input, headerSize))
        return InboundFrame(header, readExactly(input, bodySize))
    }

    private fun readExactly(input: InputStream, size: Int): ByteArray {
        val bytes = input.readNBytes(size)
        if (bytes.size != size)
            throw EOFException("expected $size bytes but received ${bytes.size}")
        return bytes
    }

    private fun frame(header: ByteArray, body: ByteArray): ByteArray =
        ByteBuffer.allocate(6 + header.size + body.size)
            .putShort(header.size.toShort())
            .putInt(body.size)
            .put(header)
            .put(body)
            .array()

    private fun json(value: String): ByteArray = "\"$value\"".toByteArray(StandardCharsets.UTF_8)

    private fun Byte.unsigned(): Int = toInt() and 0xff

    private fun Short.unsigned(): Int = toInt() and 0xffff

    companion object {
        private const val ACTOR_TYPE = "player"
        private const val BIND_PACKET = "Bind"
        private const val CLIENT_TIMEOUT_MILLIS = 3000
        private const val HEARTBEAT_PING_PACKET = "\$zlink.heartbeat.ping"
        private const val HEARTBEAT_PONG_PACKET = "\$zlink.heartbeat.pong"
        private const val MESH = "duplicate-bind"
        private const val REPLACEMENT_ATTEMPTS = 100
        private const val STREAM_NODE = "gateway"
        private val disconnectGates = ConcurrentHashMap<String, DisconnectGate>()
    }
}
