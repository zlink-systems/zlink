package systems.zlink.e2e.kotlin.observabilityops.trigger

import com.fasterxml.jackson.databind.ObjectMapper
import io.micrometer.core.instrument.Metrics
import io.micrometer.core.instrument.simple.SimpleMeterRegistry
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.nio.file.Path
import java.time.Duration
import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.future.await as awaitFuture
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import systems.zlink.contracts.messaging.Message
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.stream.connector.ZLinkStreamCodec
import systems.zlink.stream.connector.ZLinkStreamConnectionState
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload

fun main(args: Array<String>) = runBlocking {
    when {
        args.size == 3 && args[0] == "--metrics-b1" -> metricsB1(args[1], Path.of(args[2]))
        args.size == 3 && args[0] == "--reader-free-b4" -> readerFreeB4(args[1], Path.of(args[2]))
        args.size == 4 && args[0] == "--drain-watch" ->
            drainWatch(args[1], args[2], Path.of(args[3]))
        args.size == 1 -> missingPacket(args[0])
        else -> error("Usage: observability-ops-kotlin-trigger <endpoint>")
    }
}

private suspend fun missingPacket(endpoint: String) {
    val connector = connector(endpoint)
    try {
        connector.connect().await()
        try {
            connector.request(raw("ObservabilityMissingPacket", byteArrayOf(0xff.toByte(), 0)))
                .timeout(Duration.ofSeconds(5))
                .await()
            error("missing packet unexpectedly succeeded")
        } catch (expected: Exception) {
            println("OBS-A2 missing-handler reply=${expected.javaClass.simpleName}")
        }
    } finally {
        connector.close().await()
    }
}

private suspend fun metricsB1(endpoint: String, output: Path) {
    val registry = SimpleMeterRegistry()
    Metrics.addRegistry(registry)
    val connector = connector(endpoint)
    try {
        connector.connect().await()
        probeLifecycle(connector)
        repeat(3) { index ->
            val expectedCycle = index + 1
            try {
                connector.request(ForceReconnectReq(expectedCycle))
                    .timeout(Duration.ofSeconds(5))
                    .awaitReply<Unit>()
                error("force reconnect request unexpectedly received a reply")
            } catch (_: Exception) {
                // Closing the server-side session fails the pending request and starts automatic reconnect.
            }
            connector.connect().await()
            probeLifecycle(connector)
        }
        val reconnects = registry.get("zlink.stream.reconnects").counter().count()
        ObjectMapper().writeValue(output.toFile(), listOf(mapOf(
            "name" to "zlink.stream.reconnects",
            "kind" to "counter",
            "unit" to "{event}",
            "value" to reconnects,
            "count" to reconnects.toLong(),
            "tags" to emptyMap<String, String>(),
        )))
    } finally {
        connector.close().await()
        Metrics.removeRegistry(registry)
        registry.close()
    }
}

private suspend fun probeLifecycle(connector: systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector) {
    try {
        connector.request(raw("MetricsLifecycleProbe", byteArrayOf(1)))
            .timeout(Duration.ofSeconds(5))
            .await()
    } catch (_: Exception) {
        // The missing-handler response proves that the session processed the frame.
    }
}

private suspend fun readerFreeB4(endpoint: String, output: Path) {
    val connector = connector(endpoint)
    var trafficEvents = 0L
    var messagingAccurate = false
    try {
        connector.connect().await()
        repeat(8192) { index ->
            connector.send(raw("ReaderFreeTraffic", byteArrayOf((index and 0xff).toByte()))).await()
            trafficEvents++
        }
        try {
            connector.request(raw("ReaderFreeProbe", byteArrayOf(1)))
                .timeout(Duration.ofSeconds(20))
                .await()
        } catch (_: Exception) {
            messagingAccurate = true
        }
        ObjectMapper().writeValue(output.toFile(), mapOf(
            "trafficEvents" to trafficEvents,
            "messagingAccurate" to messagingAccurate,
        ))
    } finally {
        connector.close().await()
    }
}

private suspend fun drainWatch(endpoint: String, drainUrl: String, output: Path) {
    val connector = connector(endpoint)
    val disconnected = CompletableFuture<String>()
    connector.onDisconnected { event ->
        disconnected.complete(event.closeReason().name.lowercase())
        CompletableFuture.completedFuture(null)
    }.use {
        try {
            connector.connect().await()
            probeLifecycle(connector)
            val request = HttpRequest.newBuilder(URI.create(drainUrl)).GET().build()
            HttpClient.newHttpClient().send(request, HttpResponse.BodyHandlers.discarding())
            val closeReason = withTimeout(15_000) { disconnected.awaitFuture() }
            ObjectMapper().writeValue(output.toFile(), mapOf("closeReason" to closeReason))
        } finally {
            if (connector.state != ZLinkStreamConnectionState.CLOSED) connector.close().await()
        }
    }
}

private fun connector(endpoint: String) =
    ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions.createDefault(URI.create(endpoint)),
    ).kotlin()

private fun raw(name: String, bytes: ByteArray) = ZLinkStreamEncodedPayload(
    name,
    Message.from(bytes),
    emptyMap(),
    ZLinkStreamCodec.RAW,
)

private data class ForceReconnectReq(val cycle: Int)
