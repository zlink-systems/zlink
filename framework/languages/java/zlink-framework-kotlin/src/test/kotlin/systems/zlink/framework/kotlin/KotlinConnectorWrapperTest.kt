package systems.zlink.framework.kotlin

import java.lang.reflect.Proxy
import java.net.ServerSocket
import java.net.Socket
import java.net.URI
import java.nio.ByteBuffer
import java.nio.charset.StandardCharsets
import java.time.Duration
import java.time.Duration.ofMillis
import java.time.Duration.ofSeconds
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.future.await
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.yield
import org.junit.jupiter.api.Assertions
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertNotNull
import org.junit.jupiter.api.Assertions.assertNull
import org.junit.jupiter.api.Assertions.assertSame
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import systems.zlink.contracts.messaging.Message
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec as FrameworkStreamCompressionCodec
import systems.zlink.stream.connector.ZLinkStreamCloseReason
import systems.zlink.stream.connector.ZLinkStreamCompression
import systems.zlink.stream.connector.ZLinkStreamCompressionCodec
import systems.zlink.stream.connector.ZLinkStreamConnector
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload
import systems.zlink.stream.connector.ZLinkStreamError
import systems.zlink.stream.connector.ZLinkStreamErrorCode
import systems.zlink.stream.connector.ZLinkStreamException
import systems.zlink.stream.connector.ZLinkStreamMessage
import systems.zlink.stream.connector.ZLinkStreamMessageHandler
import systems.zlink.stream.connector.ZLinkStreamPacketNameResolver
import systems.zlink.stream.connector.ZLinkTypedStreamRequestCall

final class KotlinConnectorWrapperTest {
    @Test
    fun cancellingMessageFlowReleasesBufferedPayloads() = runBlocking {
        val registered = AtomicReference<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>()
        val connector =
            Proxy.newProxyInstance(
                ZLinkStreamConnector::class.java.classLoader,
                arrayOf(ZLinkStreamConnector::class.java),
            ) { _, method, arguments ->
                if (method.name == "on") {
                    @Suppress("UNCHECKED_CAST")
                    registered.set(
                        arguments[1] as ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>
                    )
                    AutoCloseable {}
                } else {
                    error("Unexpected connector call: ${method.name}")
                }
            } as ZLinkStreamConnector
        val first = CompletableDeferred<Unit>()
        val held = CompletableDeferred<Unit>()
        val collector =
            launch(start = CoroutineStart.UNDISPATCHED) {
                connector.messages("Burst").collect { message ->
                    first.complete(Unit)
                    held.await()
                    message.payload().payload().close()
                }
            }
        withTimeout(1_000) { while (registered.get() == null) yield() }
        val payloads = (1..5).map { Message.from(byteArrayOf(it.toByte())) }
        payloads.forEach { payload ->
            registered
                .get()
                .handleAsync(
                    ZLinkStreamMessage(
                        "Burst",
                        ZLinkStreamEncodedPayload("Burst", payload, mapOf()),
                        mapOf(),
                    )
                )
        }
        withTimeout(1_000) { first.await() }
        collector.cancelAndJoin()
        assertTrue(payloads.drop(1).all { it.toByteArray().isEmpty() })
    }

    @Test
    fun kotlinCompressionDslConfiguresFrameworkAndConnectorOptions() {
        val codec = PrefixCompressionCodec("kotlin:")
        val frameworkOptions = DefaultZLinkFrameworkOptions()

        frameworkOptions.configureStreamCompression { use(codec) }

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
        //  수신 한도를 기본값과 다르게 두어야 복사가 그 값을 보존하는지 볼 수 있다.
        //  `ZLinkStreamConnectorOptions`의 구성 요소 순서를 그대로 따른다.
        val connectorOptions =
            ZLinkStreamConnectorOptions(
                URI.create("tcp://127.0.0.1:7200"),
                ZLinkStreamDispatchMode.MANUAL,
                ofSeconds(1),
                ofSeconds(5),
                1,
                ofSeconds(1),
                64 * 1024,
                32 * 1024,
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

        //  Java spec 03 12: 옵션을 복사하는 확장은 지금 정의된 옵션을 모두 보존한다.
        //  수신 한도도 그중 하나다.
        assertEquals(32 * 1024, connectorOptions.maxReceivePayloadSize())
        assertEquals(32 * 1024, connectorOptions.withoutStreamCompression().maxReceivePayloadSize())
        assertEquals(
            32 * 1024,
            connectorOptions.withDefaultStreamCompression().maxReceivePayloadSize(),
        )
        assertEquals(ZLinkStreamCompression.LZ4, connectorOptions.compression())
        assertNotNull(connectorOptions.compressionCodec())
    }

    @Test
    fun kotlinRequestCompletionSurfaceUsesOnlyContractNames() {
        val extensionMethods =
            Class.forName("systems.zlink.framework.kotlin.ZLinkConnectorExtensionsKt")
                .declaredMethods

        assertTrue(extensionMethods.none { method -> method.name == "awaitTyped" })
        assertTrue(
            extensionMethods.none { method ->
                method.name == "await" &&
                    method.parameterTypes.firstOrNull() == ZLinkTypedStreamRequestCall::class.java
            }
        )
        assertTrue(
            ZLinkKotlinStreamConnector::class.java.methods.none { method ->
                method.name == "disconnect" || method.name == "reconnect"
            }
        )
    }

    @Test
    fun kotlinSendCallCarriesPacketNameMetadataAndCompress() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()

                //  Java spec 03 12: the Kotlin send builder has the same
                //  packetName/metadata/compress steps as the Java one.
                connector
                    .send(payload("Echo", "send"))
                    .packetName("Renamed")
                    .metadata("seq", "7")
                    .await()

                val sent = server.readApplicationFrame()
                assertEquals(1, sent.kind)
                assertEquals("Renamed", sent.name)
                assertEquals(0x02, sent.flags and 0x02)

                //  compress() reaches the wire as the compressed flag.
                connector.send(payload("Echo", "a".repeat(64))).compress().await()
                val compressed = server.readApplicationFrame()
                assertEquals(0x04, compressed.flags and 0x04)
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun kotlinFailuresCarryTheStreamErrorCode() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()
                //  Java spec 03 12: Kotlin adds no exception hierarchy of
                //  its own and propagates ZLinkStreamException as is.
                val failure =
                    Assertions.assertThrows(ZLinkStreamException::class.java) {
                        runBlocking {
                            connector.waitFor<String>("Never").timeout(ofMillis(50)).await()
                        }
                    }
                assertEquals(ZLinkStreamErrorCode.VALIDATION_FAILED, failure.errorCode())
            } finally {
                connector.close().await()
            }
        }
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

                val replyFuture =
                    async(Dispatchers.IO) { connector.request(payload("Echo", "request")).await() }
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
                    )
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
                val collector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        connector.messages("Push").collect { message ->
                            received.complete(message.payload())
                        }
                    }

                sendAndDispatchUntilReceived(server, connector, received)

                val payload = withTimeout(1_000) { received.await() }
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
                val collector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        connector.errors().collect { error -> received.complete(error) }
                    }

                sendErrorAndDispatchUntilReceived(server, connector, received)

                val error = withTimeout(1_000) { received.await() }
                assertEquals(ZLinkStreamErrorCode.REMOTE_ERROR, error.code())
                assertEquals("remote failed", error.message())
                collector.cancel()
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun replyReceivedFlowObservesRequestOutcomeOnManualDispatch() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()
                val observed = CompletableDeferred<String>()
                val collector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        connector.repliesReceived().collect { context ->
                            observed.complete(context.requestPacketName())
                            context.reply()?.payload()?.payload()?.close()
                        }
                    }
                yield()
                val request =
                    async(Dispatchers.IO) { connector.request(payload("Echo", "body")).await() }
                val sent = server.readApplicationFrame()
                server.sendFrame(
                    Frame(
                        kind = 3,
                        requestSeq = sent.requestSeq,
                        name = "",
                        payload = "reply".toByteArray(StandardCharsets.UTF_8),
                    )
                )
                request.await().payload().close()
                dispatchNext(connector)
                assertEquals("Echo", withTimeout(1_000) { observed.await() })
                collector.cancelAndJoin()
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun requestSendingHookAddsMetadataBeforeManualDispatch() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            val observed = CompletableDeferred<Pair<String, String?>>()
            val hook =
                connector.onRequestSending { context ->
                    observed.complete(context.requestPacketName() to context.actorId())
                    context.setMetadata("source", "kotlin")
                }
            try {
                connector.connect().await()
                val request =
                    async(Dispatchers.IO) { connector.request(payload("Echo", "body")).await() }
                val sent = server.readApplicationFrame()
                assertEquals("Echo" to null, withTimeout(1_000) { observed.await() })
                assertEquals("kotlin", sent.metadata["source"])
                server.sendFrame(
                    Frame(
                        kind = 3,
                        requestSeq = sent.requestSeq,
                        name = "",
                        payload = "reply".toByteArray(StandardCharsets.UTF_8),
                    )
                )
                request.await().payload().close()
            } finally {
                hook.close()
                connector.close().await()
            }
        }
    }

    @Test
    fun actorFlowsKeepHandleIdentityAndUnboundState() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()
                assertNull(connector.actor("missing"))
                assertTrue(connector.actors().isEmpty())

                val bound = CompletableDeferred<ZLinkKotlinStreamActor>()
                val unbound = CompletableDeferred<ZLinkKotlinStreamActor>()
                val eventOrder = java.util.Collections.synchronizedList(mutableListOf<String>())
                val boundCollector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        connector.actorBound().collect {
                            eventOrder.add("bound")
                            bound.complete(it)
                        }
                    }
                val unboundCollector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        connector.actorUnbound().collect {
                            eventOrder.add("unbound")
                            unbound.complete(it)
                        }
                    }
                yield()

                actorRegistryControl(connector, "bound", boundControl(7, "player-a"))
                val actor = connector.actor("player-a")!!
                assertSame(actor, connector.actors().single())
                assertTrue(actor.isBound)
                assertTrue(!bound.isCompleted)
                connector.dispatch().await()
                assertSame(actor, withTimeout(1_000) { bound.await() })

                actorRegistryControl(connector, "unbound", unboundControl(7))
                assertTrue(!actor.isBound)
                assertNull(connector.actor("player-a"))
                assertTrue(!unbound.isCompleted)
                connector.dispatch().await()
                assertSame(actor, withTimeout(1_000) { unbound.await() })
                assertEquals(listOf("bound", "unbound"), eventOrder)
                val stale =
                    Assertions.assertThrows(ZLinkStreamException::class.java) {
                        runBlocking { actor.send(payload("Ping", "stale")).await() }
                    }
                assertEquals(ZLinkStreamErrorCode.VALIDATION_FAILED, stale.errorCode())

                boundCollector.cancelAndJoin()
                unboundCollector.cancelAndJoin()
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun actorBoundFlowKeepsEventsWhileCollectorIsBehind() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            val eventCount = 80
            try {
                connector.connect().await()

                val firstBound = CompletableDeferred<Unit>()
                val releaseBound = CompletableDeferred<Unit>()
                val allBound = CompletableDeferred<List<String>>()
                val boundIds = mutableListOf<String>()
                val boundCollector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        connector.actorBound().collect { actor ->
                            if (boundIds.isEmpty()) {
                                firstBound.complete(Unit)
                                releaseBound.await()
                            }
                            boundIds.add(actor.actorId)
                            if (boundIds.size == eventCount) allBound.complete(boundIds.toList())
                        }
                    }
                try {
                    yield()
                    actorRegistryControl(connector, "bound", boundControl(1, "actor-1"))
                    connector.dispatch().await()
                    withTimeout(1_000) { firstBound.await() }
                    for (slot in 2..eventCount) {
                        actorRegistryControl(connector, "bound", boundControl(slot, "actor-$slot"))
                    }
                    connector.dispatch().await()
                    releaseBound.complete(Unit)
                    assertEquals(
                        (1..eventCount).map { "actor-$it" },
                        withTimeout(2_000) { allBound.await() },
                    )
                } finally {
                    boundCollector.cancelAndJoin()
                }
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun actorUnboundFlowKeepsEventsWhileCollectorIsBehind() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            val eventCount = 80
            try {
                connector.connect().await()
                for (slot in 1..eventCount) {
                    actorRegistryControl(connector, "bound", boundControl(slot, "actor-$slot"))
                }
                val firstUnbound = CompletableDeferred<Unit>()
                val releaseUnbound = CompletableDeferred<Unit>()
                val allUnbound = CompletableDeferred<List<String>>()
                val unboundIds = mutableListOf<String>()
                val unboundCollector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        connector.actorUnbound().collect { actor ->
                            if (unboundIds.isEmpty()) {
                                firstUnbound.complete(Unit)
                                releaseUnbound.await()
                            }
                            unboundIds.add(actor.actorId)
                            if (unboundIds.size == eventCount) {
                                allUnbound.complete(unboundIds.toList())
                            }
                        }
                    }
                try {
                    yield()
                    actorRegistryControl(connector, "unbound", unboundControl(1))
                    connector.dispatch().await()
                    withTimeout(1_000) { firstUnbound.await() }
                    for (slot in 2..eventCount) {
                        actorRegistryControl(connector, "unbound", unboundControl(slot))
                    }
                    connector.dispatch().await()
                    releaseUnbound.complete(Unit)
                    assertEquals(
                        (1..eventCount).map { "actor-$it" },
                        withTimeout(2_000) { allUnbound.await() },
                    )
                } finally {
                    unboundCollector.cancelAndJoin()
                }
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun connectorMessageFlowKeepsAcceptedPacketsWhileCollectorIsBehind() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            val eventCount = 80
            try {
                connector.connect().await()
                val first = CompletableDeferred<Unit>()
                val release = CompletableDeferred<Unit>()
                val all = CompletableDeferred<List<Int>>()
                val values = mutableListOf<Int>()
                val collector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        connector.messages("Burst").collect { message ->
                            if (values.isEmpty()) {
                                first.complete(Unit)
                                release.await()
                            }
                            values.add(message.payload().payload().toByteArray()[0].toInt())
                            message.payload().payload().close()
                            if (values.size == eventCount) all.complete(values.toList())
                        }
                    }
                try {
                    yield()
                    server.sendFrame(
                        Frame(1, requestSeq = null, name = "Burst", payload = byteArrayOf(1))
                    )
                    withTimeout(1_000) { while (connector.receivedCount("Burst") < 1) yield() }
                    connector.dispatch().await()
                    withTimeout(1_000) { first.await() }
                    for (value in 2..eventCount) {
                        server.sendFrame(
                            Frame(
                                1,
                                requestSeq = null,
                                name = "Burst",
                                payload = byteArrayOf(value.toByte()),
                            )
                        )
                    }
                    withTimeout(2_000) {
                        while (connector.receivedCount("Burst") < eventCount) yield()
                    }
                    connector.dispatch().await()
                    release.complete(Unit)
                    assertEquals((1..eventCount).toList(), withTimeout(2_000) { all.await() })
                } finally {
                    collector.cancelAndJoin()
                }
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun actorMessagesAndCallsUseOnlyTheirActorSlot() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()
                actorRegistryControl(connector, "bound", boundControl(7, "player-a"))
                actorRegistryControl(connector, "bound", boundControl(8, "player-b"))
                val actor = connector.actor("player-a")!!
                connector.dispatch().await()

                val received = CompletableDeferred<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>()
                val collector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        actor.messages("Ping").collect { received.complete(it) }
                    }
                yield()
                server.sendFrame(
                    Frame(
                        1,
                        requestSeq = null,
                        name = "Ping",
                        payload = byteArrayOf(8),
                        actorSlot = 8,
                    )
                )
                withTimeout(1_000) { while (connector.receivedCount("Ping") < 1) yield() }
                connector.dispatch().await()
                assertTrue(!received.isCompleted)
                server.sendFrame(
                    Frame(
                        1,
                        requestSeq = null,
                        name = "Ping",
                        payload = byteArrayOf(7),
                        actorSlot = 7,
                    )
                )
                withTimeout(1_000) { while (connector.receivedCount("Ping") < 2) yield() }
                connector.dispatch().await()
                val message = withTimeout(1_000) { received.await() }
                assertEquals("player-a", message.actorId())
                assertEquals("\u0007", message.payload().payload().toUtf8String())
                message.payload().payload().close()
                collector.cancelAndJoin()

                actor.send(payload("Ping", "send")).await()
                assertEquals(7, server.readApplicationFrame().actorSlot)

                actor.send(mapOf("value" to "typed-send")).await()
                assertEquals(7, server.readApplicationFrame().actorSlot)

                val rawRequest =
                    async(Dispatchers.IO) { actor.request(payload("Ping", "raw")).await() }
                val rawFrame = server.readApplicationFrame()
                assertEquals(7, rawFrame.actorSlot)
                server.sendFrame(
                    Frame(3, requestSeq = rawFrame.requestSeq, name = "", payload = byteArrayOf(1))
                )
                rawRequest.await().payload().close()

                val typedRequest =
                    async(Dispatchers.IO) {
                        actor.request<Map<String, String>>(mapOf("value" to "typed")).await()
                    }
                val typedFrame = server.readApplicationFrame()
                assertEquals(7, typedFrame.actorSlot)
                server.sendFrame(
                    Frame(
                        3,
                        codec = 1,
                        requestSeq = typedFrame.requestSeq,
                        name = "",
                        payload = "{\"value\":\"reply\"}".toByteArray(StandardCharsets.UTF_8),
                    )
                )
                assertEquals("reply", typedRequest.await()["value"])
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun typedMessageFlowsResolveNamesOnConnectorAndActor() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()
                actorRegistryControl(connector, "bound", boundControl(7, "player-a"))
                val actor = connector.actor("player-a")!!
                connector.dispatch().await()
                val connectorMessage =
                    CompletableDeferred<ZLinkStreamMessage<Map<String, String>>>()
                val actorMessage = CompletableDeferred<ZLinkStreamMessage<Map<String, String>>>()
                val connectorCollector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        connector.messages<Map<String, String>>().collect {
                            connectorMessage.complete(it)
                        }
                    }
                val actorCollector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        actor.messages<Map<String, String>>().collect { actorMessage.complete(it) }
                    }
                yield()
                server.sendFrame(
                    Frame(
                        kind = 1,
                        codec = 1,
                        requestSeq = null,
                        name = "Map",
                        payload = "{\"value\":\"typed\"}".toByteArray(StandardCharsets.UTF_8),
                        actorSlot = 7,
                    )
                )
                withTimeout(1_000) { while (connector.pendingDispatchCount == 0) yield() }
                assertEquals(1, connector.receivedCount("Map"))
                connector.dispatch().await()
                assertEquals(
                    "typed",
                    withTimeout(1_000) { connectorMessage.await() }.payload()["value"],
                )
                assertEquals(
                    "typed",
                    withTimeout(1_000) { actorMessage.await() }.payload()["value"],
                )

                val namedMessage = CompletableDeferred<String>()
                val namedCollector =
                    launch(start = CoroutineStart.UNDISPATCHED) {
                        actor.messages("Alias", String::class).collect {
                            namedMessage.complete(it.payload())
                        }
                    }
                yield()
                server.sendFrame(
                    Frame(
                        kind = 1,
                        codec = 1,
                        requestSeq = null,
                        name = "Alias",
                        payload = "\"named\"".toByteArray(StandardCharsets.UTF_8),
                        actorSlot = 7,
                    )
                )
                withTimeout(1_000) { while (connector.receivedCount("Alias") == 0) yield() }
                connector.dispatch().await()
                assertEquals("named", withTimeout(1_000) { namedMessage.await() })

                connectorCollector.cancelAndJoin()
                actorCollector.cancelAndJoin()
                namedCollector.cancelAndJoin()
            } finally {
                connector.close().await()
            }
        }
    }

    @Test
    fun explicitPacketNamePrecedesRejectingResolverForTypedCalls() = runBlocking {
        TcpServer().use { server ->
            val resolver = ZLinkStreamPacketNameResolver {
                throw IllegalArgumentException("reject")
            }
            val connector =
                ZLinkStreamConnectorFactory.create(options(server.endpoint(), resolver)).kotlin()
            try {
                connector.connect().await()
                actorRegistryControl(connector, "bound", boundControl(7, "player-a"))
                val actor = connector.actor("player-a")!!
                connector.dispatch().await()
                val payload = mapOf("value" to "typed")

                assertTrue(runCatching { connector.send(payload).await() }.isFailure)

                connector.send(payload).packetName("ExplicitSend").await()
                assertEquals("ExplicitSend", server.readApplicationFrame().name)
                actor.send(payload).packetName("ActorSend").await()
                assertEquals("ActorSend", server.readApplicationFrame().name)

                val connectorRequest =
                    async(Dispatchers.IO) {
                        connector
                            .request<Map<String, String>>(payload)
                            .packetName("ExplicitRequest")
                            .await()
                    }
                val sent = server.readApplicationFrame()
                assertEquals("ExplicitRequest", sent.name)
                server.sendFrame(
                    Frame(
                        kind = 3,
                        codec = 1,
                        requestSeq = sent.requestSeq,
                        name = "",
                        payload = "{\"value\":\"reply\"}".toByteArray(StandardCharsets.UTF_8),
                    )
                )
                assertEquals("reply", connectorRequest.await()["value"])

                val actorRequest =
                    async(Dispatchers.IO) {
                        actor
                            .request<Map<String, String>>(payload)
                            .packetName("ActorRequest")
                            .await()
                    }
                val actorSent = server.readApplicationFrame()
                assertEquals("ActorRequest", actorSent.name)
                assertEquals(7, actorSent.actorSlot)
                server.sendFrame(
                    Frame(
                        kind = 3,
                        codec = 1,
                        requestSeq = actorSent.requestSeq,
                        name = "",
                        payload = "{\"value\":\"reply\"}".toByteArray(StandardCharsets.UTF_8),
                    )
                )
                assertEquals("reply", actorRequest.await()["value"])
            } finally {
                connector.close().await()
            }
        }
    }

    private fun actorRegistryControl(
        connector: ZLinkKotlinStreamConnector,
        action: String,
        payload: ByteArray,
    ) {
        val registryField = connector.inner.javaClass.getDeclaredField("actorRegistry")
        registryField.isAccessible = true
        val registry = registryField.get(connector.inner)
        val method = registry.javaClass.getDeclaredMethod(action, ByteArray::class.java)
        method.isAccessible = true
        method.invoke(registry, payload)
    }

    private fun boundControl(slot: Int, actorId: String): ByteArray {
        val id = actorId.toByteArray(StandardCharsets.UTF_8)
        return ByteBuffer.allocate(4 + id.size)
            .put(1)
            .putShort(slot.toShort())
            .put(id.size.toByte())
            .put(id)
            .array()
    }

    private fun unboundControl(slot: Int): ByteArray =
        ByteBuffer.allocate(3).put(1).putShort(slot.toShort()).array()

    @Test
    fun kotlinObservationBuildersPreserveJavaQueueSemantics() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            try {
                connector.connect().await()

                connector
                    .expectNone<Map<String, String>>("Notice")
                    .within(Duration.ofMillis(25))
                    .await()

                val sequence =
                    async(start = CoroutineStart.UNDISPATCHED) {
                        connector
                            .waitForSequence<Map<String, String>>("Notice")
                            .expect { it.payload()["value"] == "first" }
                            .expect { it.payload()["value"] == "second" }
                            .timeout(ofSeconds(1))
                            .await()
                    }
                server.sendFrame(
                    Frame(
                        kind = 1,
                        codec = 1,
                        requestSeq = null,
                        name = "Notice",
                        payload = "{\"value\":\"first\"}".toByteArray(StandardCharsets.UTF_8),
                    )
                )
                server.sendFrame(
                    Frame(
                        kind = 1,
                        codec = 1,
                        requestSeq = null,
                        name = "Notice",
                        payload = "{\"value\":\"second\"}".toByteArray(StandardCharsets.UTF_8),
                    )
                )

                assertEquals(
                    listOf("first", "second"),
                    sequence.await().map { it.payload()["value"] },
                )

                val unexpected =
                    async(start = CoroutineStart.UNDISPATCHED) {
                        runCatching {
                                connector
                                    .expectNone<Map<String, String>>("Notice")
                                    .within(ofSeconds(1))
                                    .await()
                            }
                            .exceptionOrNull()
                    }
                server.sendFrame(
                    Frame(
                        kind = 1,
                        codec = 1,
                        requestSeq = null,
                        name = "Notice",
                        payload = "{\"value\":\"unexpected\"}".toByteArray(StandardCharsets.UTF_8),
                    )
                )
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

                val failure =
                    runCatching {
                            connector
                                .waitFor<Map<String, String>>("Late")
                                .timeout(ofMillis(25))
                                .await()
                        }
                        .exceptionOrNull()
                assertTrue(failure != null)

                server.sendFrame(
                    Frame(
                        kind = 1,
                        codec = 1,
                        requestSeq = null,
                        name = "Late",
                        payload =
                            "{\"value\":\"after-timeout\"}".toByteArray(StandardCharsets.UTF_8),
                    )
                )
                withTimeout(1_000) {
                    while (connector.receivedCount("Late") == 0) {
                        yield()
                    }
                }

                val message =
                    connector.waitFor<Map<String, String>>("Late").timeout(ofSeconds(1)).await()
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

                val cancelled =
                    async(start = CoroutineStart.UNDISPATCHED) {
                        connector
                            .waitFor<Map<String, String>>("Cancelled")
                            .timeout(ofSeconds(1))
                            .await()
                    }
                cancelled.cancelAndJoin()

                server.sendFrame(
                    Frame(
                        kind = 1,
                        codec = 1,
                        requestSeq = null,
                        name = "Cancelled",
                        payload = "{\"value\":\"after-cancel\"}".toByteArray(StandardCharsets.UTF_8),
                    )
                )
                withTimeout(1_000) {
                    while (connector.receivedCount("Cancelled") == 0) {
                        yield()
                    }
                }

                val message =
                    connector
                        .waitFor<Map<String, String>>("Cancelled")
                        .timeout(ofSeconds(1))
                        .await()
                assertEquals("after-cancel", message.payload()["value"])
            } finally {
                connector.close().await()
            }
        }
    }

    //  #600. The wrapper is the only surface a Kotlin caller holds, so a
    //  reason or a wait form that lives only on the Java connector is out of
    //  reach for that caller. Each assertion below fails if the wrapper
    //  member it exercises is removed: the two type-only waits stop
    //  compiling, and closeReason() has no substitute the wrapper offers.
    @Test
    fun kotlinConnectorReadsTheCloseReasonFromTheWrapper() = runBlocking {
        TcpServer().use { server ->
            val connector = ZLinkStreamConnectorFactory.create(options(server.endpoint())).kotlin()
            //  Common spec 32 §6.2: never ended means no reason yet.
            assertNull(connector.closeReason())
            connector.connect().await()
            assertNull(connector.closeReason())

            connector.close().await()
            //  The reason is recorded on the disconnect notification, which
            //  close() does not order itself against, so read it until it
            //  settles rather than on the first look.
            withTimeout(2_000) {
                while (connector.closeReason() == null) {
                    yield()
                }
            }
            assertEquals(ZLinkStreamCloseReason.CLIENT_CLOSE, connector.closeReason())
        }
    }

    @Test
    fun kotlinAssertionsUseJavaFailureClassification() = runBlocking {
        ZLinkKotlinStreamAssert.ensure(true, "condition should pass")
        val timeout =
            ZLinkKotlinStreamAssert.expectFailure("REQUEST_TIMEOUT") {
                throw TimeoutException("request timed out")
            }
        assertEquals(ZLinkStreamErrorCode.REQUEST_TIMEOUT, timeout.code())
        ZLinkKotlinStreamAssert.expectTimeout { throw TimeoutException("request timed out") }
    }

    private fun options(
        endpoint: URI = URI.create("tcp://127.0.0.1:7200"),
        nameResolver: ZLinkStreamPacketNameResolver? = null,
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
            nameResolver,
            null,
        )

    private fun payload(packetName: String, body: String) =
        ZLinkStreamEncodedPayload(packetName, Message.from(body), mapOf())

    private class PrefixCompressionCodec(private val prefix: String) :
        ZLinkStreamCompressionCodec, FrameworkStreamCompressionCodec {
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
                )
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
                    payload =
                        """{"code":"remote","message":"remote failed"}"""
                            .toByteArray(StandardCharsets.UTF_8),
                )
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
        @Volatile private var current: Socket? = null
        private val acceptThread =
            Thread {
                    while (!server.isClosed) {
                        try {
                            sockets.add(server.accept())
                        } catch (_: Exception) {
                            if (!server.isClosed) {
                                throw RuntimeException("accept failed")
                            }
                        }
                    }
                }
                .apply {
                    name = "zlink-kotlin-connector-test-server"
                    isDaemon = true
                    start()
                }

        fun endpoint(): URI = URI.create("tcp://127.0.0.1:${server.localPort}")

        fun readFrame(): Frame {
            val input = socket().getInputStream()
            val prefix = input.readNBytes(6)
            val headerLength = ((prefix[0].toInt() and 0xff) shl 8) or (prefix[1].toInt() and 0xff)
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
            val prefix =
                ByteBuffer.allocate(6).putShort(header.size.toShort()).putInt(payload.size).array()
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
            return sockets.poll(5, TimeUnit.SECONDS)?.also { current = it }
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
            val metadata = mutableMapOf<String, String>()
            if ((flags and 0x02) != 0) {
                val metadataLength = buffer.short.toInt() and 0xffff
                val end = buffer.position() + metadataLength
                repeat(buffer.get().toInt() and 0xff) {
                    val key = ByteArray(buffer.get().toInt() and 0xff)
                    buffer.get(key)
                    val value = ByteArray(buffer.short.toInt() and 0xffff)
                    buffer.get(value)
                    metadata[String(key, StandardCharsets.UTF_8)] =
                        String(value, StandardCharsets.UTF_8)
                }
                assertEquals(end, buffer.position())
            }
            if ((flags and 0x08) != 0) {
                val correlationLength = buffer.get().toInt() and 0xff
                buffer.position(buffer.position() + correlationLength)
            }
            if ((flags and 0x10) != 0) buffer.position(buffer.position() + 37)
            val actorSlot = if ((flags and 0x20) != 0) buffer.short.toInt() and 0xffff else null
            return Frame(
                kind = kind,
                codec = codec,
                flags = flags,
                requestSeq = requestSeq,
                name = String(nameBytes, StandardCharsets.UTF_8),
                payload = ByteArray(0),
                actorSlot = actorSlot,
                metadata = metadata,
            )
        }

        private fun encodeHeader(frame: Frame): ByteArray {
            val name = frame.name.toByteArray(StandardCharsets.UTF_8)
            val flags =
                (if (frame.requestSeq == null) 0 else 0x01) or
                    (if (frame.actorSlot == null) 0 else 0x20)
            val buffer =
                ByteBuffer.allocate(
                    4 +
                        (if (frame.requestSeq == null) 0 else 8) +
                        1 +
                        name.size +
                        (if (frame.actorSlot == null) 0 else 2)
                )
            buffer.put(0xF2.toByte())
            buffer.put(frame.kind.toByte())
            buffer.put(frame.codec.toByte())
            buffer.put(flags.toByte())
            if (frame.requestSeq != null) {
                buffer.putLong(frame.requestSeq)
            }
            buffer.put(name.size.toByte())
            buffer.put(name)
            if (frame.actorSlot != null) buffer.putShort(frame.actorSlot.toShort())
            return buffer.array()
        }
    }

    private data class Frame(
        val kind: Int,
        val codec: Int = 0,
        val flags: Int = 0,
        val requestSeq: Long?,
        val name: String,
        val payload: ByteArray,
        val actorSlot: Int? = null,
        val metadata: Map<String, String> = mapOf(),
    )
}
