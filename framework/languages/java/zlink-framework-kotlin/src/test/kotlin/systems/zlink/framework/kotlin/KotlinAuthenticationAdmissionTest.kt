package systems.zlink.framework.kotlin

import java.net.URI
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.yield
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Test
import systems.zlink.contracts.messaging.Message
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.monitoring.ZLinkListenerKind
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.testsupport.ZLinkHostServing
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload

class KotlinAuthenticationAdmissionTest {
    @Test
    fun suspendedLoginHoldsAdmissionUntilReplySubmission() = runBlocking {
        val gate = HandlerGate()
        handlerGate = gate
        val options = DefaultZLinkFrameworkOptions()
        options
            .addStreamNode("clients")
            .bind(0)
            .registerSession<AdmissionSession>()
            .authenticationPacketName("Login")
            .maxConcurrentAuthentications(1)
            .authenticationRetryDelay(Duration.ofMillis(20))

        ZLinkFrameworkRuntimeTestAccess.startHost(options, ZLinkJavaBackendAdapterFactory()).use {
            runtime ->
            ZLinkHostServing.await(runtime)
            val endpoint =
                URI.create(runtime.listenerStatus(ZLinkListenerKind.STREAM, "clients").endpoint())
            val connectorOptions = ZLinkStreamConnectorOptions.createDefault(endpoint)
            val first = ZLinkStreamConnectorFactory.create(connectorOptions).kotlin()
            val second = ZLinkStreamConnectorFactory.create(connectorOptions).kotlin()
            try {
                first.connect().await()
                second.connect().await()
                withTimeout(5_000) {
                    val requestA = async(start = CoroutineStart.UNDISPATCHED) { login(first) }
                    gate.entered.await()
                    val waiting = CompletableDeferred<Unit>()
                    second
                        .onAdmissionWaiting {
                            waiting.complete(Unit)
                            CompletableFuture.completedFuture(null)
                        }
                        .use {
                            val requestB =
                                async(start = CoroutineStart.UNDISPATCHED) { login(second) }
                            while (!waiting.isCompleted) {
                                second.dispatch().await()
                                yield()
                            }
                            assertEquals(1, gate.calls.get())
                            gate.reply.complete(Unit)
                            assertEquals("\"accepted\"", requestA.await())
                            assertEquals("\"accepted\"", requestB.await())
                            assertEquals(2, gate.calls.get())
                        }
                }
            } finally {
                gate.reply.complete(Unit)
                gate.finish.complete(Unit)
                first.close().await()
                second.close().await()
            }
        }
    }

    class AdmissionSession(private val session: ZLinkSessionContext) : ZLinkSuspendingSession() {
        override fun context(): ZLinkSessionContext = session

        override suspend fun onDispatchSuspending(
            dispatch: ZLinkSessionDispatchContext,
            payload: ZLinkMessage,
        ) {
            val gate = handlerGate
            val first = gate.calls.incrementAndGet() == 1
            if (first) {
                gate.entered.complete(Unit)
                gate.reply.await()
            }
            session.client().kotlin().reply("accepted").await()
            if (first) gate.finish.await()
        }
    }

    private companion object {
        lateinit var handlerGate: HandlerGate

        suspend fun login(connector: ZLinkKotlinStreamConnector): String =
            Message.from("credentials").use { body ->
                connector
                    .request(ZLinkStreamEncodedPayload("Login", body, mapOf()))
                    .await()
                    .payload()
                    .use { it.toUtf8String() }
            }
    }

    private class HandlerGate {
        val calls = AtomicInteger()
        val entered = CompletableDeferred<Unit>()
        val reply = CompletableDeferred<Unit>()
        val finish = CompletableDeferred<Unit>()
    }
}
