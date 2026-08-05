package systems.zlink.framework.kotlin

import java.net.URI
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.charset.StandardCharsets
import java.time.Duration.ofMillis
import java.time.Duration.ofSeconds
import java.util.concurrent.CompletionException
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.future.await
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.yield
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertSame
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import systems.zlink.contracts.messaging.Message
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec as FrameworkStreamCompressionCodec
import systems.zlink.stream.connector.ZLinkStreamCompression
import systems.zlink.stream.connector.ZLinkStreamCompressionCodec
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload
import systems.zlink.stream.connector.ZLinkStreamError
import systems.zlink.stream.connector.ZLinkStreamErrorCode
import systems.zlink.stream.connector.ZLinkTypedStreamRequestCall

final class KotlinConnectorWrapperTest {
    @Test
    fun kotlinCompressionDslConfiguresFrameworkAndConnectorOptions() {
        val codec = PrefixCompressionCodec("kotlin:")
        val frameworkOptions = DefaultZLinkFrameworkOptions()

        frameworkOptions.configureStreamCompression {
            use(codec)
        }

        assertSame(codec, frameworkOptions.registration().streamCompressionCodec())

        val connectorOptions = options().withStreamCompression(codec)
        assertEquals(ZLinkStreamCompression.LZ4, connectorOptions.compression())
        assertSame(codec, connectorOptions.compressionCodec())

        val disabled = connectorOptions.withoutStreamCompression()
        assertEquals(ZLinkStreamCompression.NONE, disabled.compression())
        assertEquals(null, disabled.compressionCodec())
    }

    @Test
    fun kotlinCompressionDslPreservesReceivedMessageLimit() {
        val connectorOptions = ZLinkStreamConnectorOptions(
            URI.create("tcp://127.0.0.1:7200"),
            ZLinkStreamDispatchMode.MANUAL,
            ofSeconds(1),
            1,
            ofSeconds(1),
            64 * 1024,
            64 * 1024,
            7,
            true,
            ofSeconds(1),
            ofSeconds(5),
            true,
            ofMillis(250),
            ofSeconds(5),
            2.0,
            false,
            ZLinkStreamCompression.LZ4,
            null,
            null,
        )

        assertEquals(7, connectorOptions.withDefaultStreamCompression().maxReceivedMessages())
        assertEquals(7, connectorOptions.withoutStreamCompression().maxReceivedMessages())
    }

    @Test
    fun kotlinCompressionDslPreservesInboundObserverLimits() {
        val connectorOptions = ZLinkStreamConnectorOptions(
            URI.create("tcp://127.0.0.1:7200"),
            ZLinkStreamDispatchMode.MANUAL,
            ofSeconds(1),
            ofSeconds(2),
            3,
            ofSeconds(1),
            64 * 1024,
            64 * 1024,
            7,
            11,
            13,
            true,
            ofSeconds(1),
            ofSeconds(5),
            true,
            ofMillis(250),
            ofSeconds(5),
            2.0,
            false,
            ZLinkStreamCompression.LZ4,
            null,
            null,
            null,
        )

        val disabled = connectorOptions.withoutStreamCompression()
        assertEquals(11, disabled.maxInboundObserverNotifications())
        assertEquals(13, disabled.maxInboundObserverPayloadPreviewBytes())
        assertEquals(ZLinkStreamCompression.NONE, disabled.compression())
    }

    @Test
    fun kotlinRequestCompletionSurfaceUsesOnlyContractNames() {
        val extensionMethods = Class.forName(
            "systems.zlink.framework.kotlin.ZLinkConnectorExtensionsKt",
        ).declaredMethods

        assertTrue(extensionMethods.none { method ->
            method.name == "awaitTyped"
        })
        assertTrue(extensionMethods.none { method ->
            method.name == "await" &&
                method.parameterTypes.firstOrNull() == ZLinkTypedStreamRequestCall::class.java
        })
        assertTrue(ZLinkKotlinStreamConnector::class.java.methods.none { method ->
            method.name == "disconnect" || method.name == "reconnect"
        })
    }

    @Test
    fun kotlinWrapperPreservesConnectorSemantics() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()
                assertTrue(connector.isConnected)

                connector.send(payload("Echo", "send")).await()
                val sent = server.readApplicationFrame()
                assertEquals(1, sent.kind)
                assertEquals("Echo", sent.name)
                assertEquals("send", sent.payload.toString(StandardCharsets.UTF_8))

                val replyFuture = async(Dispatchers.IO) {
                    connector.request(payload("Echo", "request")).await()
                }
                val request = server.readApplicationFrame()
                assertEquals(2, request.kind)
                assertEquals("Echo", request.name)
                assertEquals("request", request.payload.toString(StandardCharsets.UTF_8))
                server.sendFrame(
                Frame(
                    kind = 3,
                    requestSeq = request.requestSeq,
                    name = "",
                    payload = "reply".toByteArray(StandardCharsets.UTF_8),
                ),
                )

                val reply = replyFuture.await()
                try {
                    assertEquals("reply", reply.payload().toUtf8String())
                } finally {
                    reply.payload().close()
                }

            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun connectorMessagesFlowUsesJavaManualDispatchSemantics() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()

                val received = CompletableDeferred<ZLinkStreamEncodedPayload>()
                val collector = launch(start = CoroutineStart.UNDISPATCHED) {
                    connector.messages("Push").collect { message ->
                        received.complete(message.payload())
                    }
                }

                sendAndDispatchUntilReceived(server, connector, received)

                val payload = withTimeout(1_000) {
                    received.await()
                }
                try {
                    assertEquals("Push", payload.packetName())
                    assertEquals("event", payload.payload().toUtf8String())
                } finally {
                    payload.payload().close()
                    collector.cancel()
                }
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun connectorErrorsFlowUsesJavaManualDispatchSemantics() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()

                val received = CompletableDeferred<ZLinkStreamError>()
                val collector = launch(start = CoroutineStart.UNDISPATCHED) {
                    connector.errors().collect { error ->
                        received.complete(error)
                    }
                }

                sendErrorAndDispatchUntilReceived(server, connector, received)

                val error = withTimeout(1_000) {
                    received.await()
                }
                assertEquals(ZLinkStreamErrorCode.REMOTE_ERROR, error.code())
                assertEquals("remote failed", error.message())
                collector.cancel()
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun boundedQueueDropsNewestProcessMessageOnceAndRetainsOlderMessage() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(
                options(server.endpoint(), maxReceivedMessages = 1),
            ).kotlin()
            try {
                connector.connect().await()
                val dropped = CompletableDeferred<ZLinkStreamError>()
                val collector = launch(start = CoroutineStart.UNDISPATCHED) {
                    connector.errors().collect { error ->
                        if (error.code() == ZLinkStreamErrorCode.RECEIVED_MESSAGE_DROPPED) {
                            dropped.complete(error)
                        }
                    }
                }

                server.sendFrame(Frame(
                    kind = 1,
                    codec = 1,
                    requestSeq = null,
                    name = "Drop",
                    payload = "\"first\"".toByteArray(StandardCharsets.UTF_8),
                ))
                server.sendFrame(Frame(
                    kind = 1,
                    codec = 1,
                    requestSeq = null,
                    name = "Drop",
                    payload = "\"second\"".toByteArray(StandardCharsets.UTF_8),
                ))

                withTimeout(1_000) {
                    while (connector.pendingDispatchCount < 2) {
                        yield()
                    }
                }
                connector.dispatch().await()
                assertEquals(ZLinkStreamErrorCode.RECEIVED_MESSAGE_DROPPED, dropped.await().code())

                val retained = connector.waitFor<String>("Drop")
                    .timeout(ofSeconds(1))
                    .await()
                assertEquals("first", retained.payload())
                assertEquals(0, connector.pendingDispatchCount)
                collector.cancel()
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun kotlinObservationBuildersPreserveJavaQueueSemantics() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()

                connector.expectNone<Map<String, String>>("Notice")
                    .within(java.time.Duration.ofMillis(25))
                    .await()

                val sequence = async(start = CoroutineStart.UNDISPATCHED) {
                    connector.waitForSequence<Map<String, String>>("Notice")
                        .expect { it.payload()["value"] == "first" }
                        .expect { it.payload()["value"] == "second" }
                        .timeout(ofSeconds(1))
                        .await()
                }
                server.sendFrame(Frame(
                    kind = 1,
                    codec = 1,
                    requestSeq = null,
                    name = "Notice",
                    payload = "{\"value\":\"first\"}".toByteArray(StandardCharsets.UTF_8),
                ))
                server.sendFrame(Frame(
                    kind = 1,
                    codec = 1,
                    requestSeq = null,
                    name = "Notice",
                    payload = "{\"value\":\"second\"}".toByteArray(StandardCharsets.UTF_8),
                ))

                assertEquals(listOf("first", "second"), sequence.await().map { it.payload()["value"] })

                val unexpected = async(start = CoroutineStart.UNDISPATCHED) {
                    runCatching {
                        connector.expectNone<Map<String, String>>("Notice")
                            .within(ofSeconds(1))
                            .await()
                    }.exceptionOrNull()
                }
                server.sendFrame(Frame(
                    kind = 1,
                    codec = 1,
                    requestSeq = null,
                    name = "Notice",
                    payload = "{\"value\":\"unexpected\"}".toByteArray(StandardCharsets.UTF_8),
                ))
                assertTrue(unexpected.await() != null)
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun timedOutWaiterDoesNotConsumeALaterMessage() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()

                val failure = runCatching {
                    connector.waitFor<Map<String, String>>("Late")
                        .timeout(ofMillis(25))
                        .await()
                }.exceptionOrNull()
                assertTrue(failure != null)

                server.sendFrame(Frame(
                    kind = 1,
                    codec = 1,
                    requestSeq = null,
                    name = "Late",
                    payload = "{\"value\":\"after-timeout\"}"
                        .toByteArray(StandardCharsets.UTF_8),
                ))
                withTimeout(1_000) {
                    while (connector.pendingDispatchCount == 0) {
                        yield()
                    }
                }

                val message = connector.waitFor<Map<String, String>>("Late")
                    .timeout(ofSeconds(1))
                    .await()
                assertEquals("after-timeout", message.payload()["value"])
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun cancelledWaiterDoesNotConsumeALaterMessage() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()

                val cancelled = async(start = CoroutineStart.UNDISPATCHED) {
                    connector.waitFor<Map<String, String>>("Cancelled")
                        .timeout(ofSeconds(1))
                        .await()
                }
                cancelled.cancelAndJoin()

                server.sendFrame(Frame(
                    kind = 1,
                    codec = 1,
                    requestSeq = null,
                    name = "Cancelled",
                    payload = "{\"value\":\"after-cancel\"}"
                        .toByteArray(StandardCharsets.UTF_8),
                ))
                withTimeout(1_000) {
                    while (connector.pendingDispatchCount == 0) {
                        yield()
                    }
                }

                val message = connector.waitFor<Map<String, String>>("Cancelled")
                    .timeout(ofSeconds(1))
                    .await()
                assertEquals("after-cancel", message.payload()["value"])
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun kotlinAssertionsUseJavaFailureClassification() = runBlocking {
        ZLinkKotlinStreamAssert.ensure(true, "condition should pass")
        val timeout = ZLinkKotlinStreamAssert.expectFailure("REQUEST_TIMEOUT") {
            throw java.util.concurrent.TimeoutException("request timed out")
        }
        assertEquals(ZLinkStreamErrorCode.REQUEST_TIMEOUT, timeout.code())
        ZLinkKotlinStreamAssert.expectTimeout {
            throw java.util.concurrent.TimeoutException("request timed out")
        }
    }

    private fun options(
        endpoint: URI = URI.create("tcp://127.0.0.1:7200"),
        maxReceivedMessages: Int = 1024,
    ) =
        ZLinkStreamConnectorOptions(
            endpoint,
            ZLinkStreamDispatchMode.MANUAL,
            ofSeconds(1),
            ofSeconds(1),
            1,
            ofSeconds(1),
            64 * 1024,
            64 * 1024,
            maxReceivedMessages,
            1024,
            0,
            true,
            ofSeconds(1),
            ofSeconds(5),
            true,
            ofMillis(250),
            ofSeconds(5),
            2.0,
            false,
            ZLinkStreamCompression.LZ4,
            null,
            null,
            null,
        )

    private fun payload(packetName: String, body: String) =
        ZLinkStreamEncodedPayload(packetName, Message.from(body), mapOf())

    private class PrefixCompressionCodec(private val prefix: String) :
        ZLinkStreamCompressionCodec,
        FrameworkStreamCompressionCodec {
        override fun compress(payload: ByteArray): ByteArray =
            (prefix + payload.toString(StandardCharsets.UTF_8)).toByteArray(StandardCharsets.UTF_8)

        override fun decompress(payload: ByteArray, maxDecompressedSize: Int): ByteArray {
            val text = payload.toString(StandardCharsets.UTF_8)
            require(text.startsWith(prefix)) { "unexpected compression marker" }
            val decoded = text.removePrefix(prefix).toByteArray(StandardCharsets.UTF_8)
            require(decoded.size <= maxDecompressedSize) { "decompressed payload is too large" }
            return decoded
        }
    }

    private suspend fun sendAndDispatchUntilReceived(
        server: TcpServer,
        connector: ZLinkKotlinStreamConnector,
        received: CompletableDeferred<*>,
    ) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (System.nanoTime() < deadline) {
            server.sendFrame(
                Frame(
                    kind = 1,
                    requestSeq = null,
                    name = "Push",
                    payload = "event".toByteArray(StandardCharsets.UTF_8),
                ),
            )
            connector.dispatch().await()
            yield()
            if (received.isCompleted) {
                return
            }
            Thread.onSpinWait()
        }
        throw AssertionError("flow message was not received within 5s")
    }

    private suspend fun dispatchNext(connector: ZLinkKotlinStreamConnector) {
        withTimeout(1_000) {
            while (connector.pendingDispatchCount == 0) {
                yield()
            }
        }
        connector.dispatch().await()
    }

    private suspend fun sendErrorAndDispatchUntilReceived(
        server: TcpServer,
        connector: ZLinkKotlinStreamConnector,
        received: CompletableDeferred<*>,
    ) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (System.nanoTime() < deadline) {
            server.sendFrame(
                Frame(
                    kind = 4,
                    codec = 1,
                    requestSeq = null,
                    name = "",
                    payload = """{"code":"remote","message":"remote failed"}"""
                        .toByteArray(StandardCharsets.UTF_8),
                ),
            )
            connector.dispatch().await()
            yield()
            if (received.isCompleted) {
                return
            }
            Thread.onSpinWait()
        }
        throw AssertionError("flow error was not received within 5s")
    }

    private class TcpServer : AutoCloseable {
        private val server = ServerSocket(0)
        private val sockets = LinkedBlockingQueue<Socket>()
        @Volatile
        private var current: Socket? = null
        private val acceptThread = Thread {
            while (!server.isClosed) {
                try {
                    sockets.add(server.accept())
                } catch (_: Exception) {
                    if (!server.isClosed) {
                        throw RuntimeException("accept failed")
                    }
                }
            }
        }.apply {
            name = "zlink-kotlin-connector-test-server"
            isDaemon = true
            start()
        }

        fun endpoint(): URI = URI.create("tcp://127.0.0.1:${server.localPort}")

        fun readFrame(): Frame {
            val input = socket().getInputStream()
            val prefix = input.readNBytes(6)
            val headerLength = ((prefix[0].toInt() and 0xff) shl 8) or
                (prefix[1].toInt() and 0xff)
            val payloadLength = ByteBuffer.wrap(prefix, 2, 4).int
            val header = input.readNBytes(headerLength)
            val payload = input.readNBytes(payloadLength)
            return decodeHeader(header).copy(payload = payload)
        }

        fun readApplicationFrame(): Frame {
            while (true) {
                val frame = readFrame()
                if (frame.kind != 5) {
                    return frame
                }
            }
        }

        fun sendFrame(frame: Frame) {
            val header = encodeHeader(frame)
            sendRawFrame(header, frame.payload)
        }

        fun sendRawFrame(header: ByteArray, payload: ByteArray) {
            val output = socket().getOutputStream()
            val prefix = ByteBuffer.allocate(6)
                .putShort(header.size.toShort())
                .putInt(payload.size)
                .array()
            output.write(prefix)
            output.write(header)
            output.write(payload)
            output.flush()
        }

        override fun close() {
            current?.close()
            server.close()
            acceptThread.interrupt()
        }

        private fun socket(): Socket {
            val existing = current
            if (existing != null && !existing.isClosed) {
                return existing
            }
            return sockets.poll(5, TimeUnit.SECONDS)
                ?.also { current = it }
                ?: throw AssertionError("client did not connect within 5s")
        }

        private fun decodeHeader(header: ByteArray): Frame {
            val buffer = ByteBuffer.wrap(header)
            assertEquals(0xF2, buffer.get().toInt() and 0xff)
            val kind = buffer.get().toInt() and 0xff
            val codec = buffer.get().toInt() and 0xff
            val flags = buffer.get().toInt() and 0xff
            val requestSeq = if ((flags and 0x01) != 0) buffer.long else null
            val nameLength = buffer.get().toInt() and 0xff
            val nameBytes = ByteArray(nameLength)
            buffer.get(nameBytes)
            return Frame(
                kind = kind,
                codec = codec,
                requestSeq = requestSeq,
                name = String(nameBytes, StandardCharsets.UTF_8),
                payload = ByteArray(0),
            )
        }

        private fun encodeHeader(frame: Frame): ByteArray {
            val name = frame.name.toByteArray(StandardCharsets.UTF_8)
            val flags = if (frame.requestSeq == null) 0 else 0x01
            val buffer = ByteBuffer.allocate(4 + (if (frame.requestSeq == null) 0 else 8) + 1 + name.size)
            buffer.put(0xF2.toByte())
            buffer.put(frame.kind.toByte())
            buffer.put(frame.codec.toByte())
            buffer.put(flags.toByte())
            if (frame.requestSeq != null) {
                buffer.putLong(frame.requestSeq)
            }
            buffer.put(name.size.toByte())
            buffer.put(name)
            return buffer.array()
        }
    }

    private data class Frame(
        val kind: Int,
        val codec: Int = 0,
        val requestSeq: Long?,
        val name: String,
        val payload: ByteArray,
    )
}
