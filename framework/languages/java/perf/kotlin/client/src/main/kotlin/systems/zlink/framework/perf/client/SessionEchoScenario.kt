package systems.zlink.framework.perf.client

import java.net.URI
import java.time.Duration
import java.util.LinkedHashMap
import java.util.concurrent.CompletionStage
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLongArray
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.request
import systems.zlink.framework.perf.DecimalText
import systems.zlink.framework.perf.EndpointManifest
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.PayloadPattern
import systems.zlink.framework.perf.PerfClock
import systems.zlink.framework.perf.PerfEchoReply
import systems.zlink.framework.perf.kotlin.completionStage
import systems.zlink.stream.connector.ZLinkStreamConnector
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode

// §10.1, §10.2 and §11.1 share the same connector workload. One Kotlin STREAM request wrapper call
// immediately before each operation is measured through typed echo identity and payload validation.
class SessionEchoScenario(
    private val manifest: EndpointManifest,
    private val measurement: Measurement,
    private val index: Int,
) : ClientControl.Workload {
    private data class Connected(val id: Int, val connector: ZLinkStreamConnector)

    private val connectors = CopyOnWriteArrayList<Connected>()
    private val owned = CopyOnWriteArrayList<ZLinkStreamConnector>()
    private lateinit var sequences: AtomicLongArray

    private fun first(): Int {
        val total = manifest.workload().connections()
        val clients = manifest.workload().clientCount()
        return index * (total / clients) + minOf(index, total % clients)
    }

    private fun count(): Int {
        val total = manifest.workload().connections()
        val clients = manifest.workload().clientCount()
        return total / clients + if (index < total % clients) 1 else 0
    }

    private fun create(endpoint: URI): ZLinkStreamConnector {
        val workload = manifest.workload()
        val defaults = ZLinkStreamConnectorOptions.createDefault(endpoint)
        val options = ZLinkStreamConnectorOptions(
            endpoint,
            ZLinkStreamDispatchMode.IMMEDIATE,
            Duration.ofMillis(workload.requestTimeoutMs().toLong()),
            defaults.waitTimeout(),
            defaults.maxReconnectAttempts(),
            Duration.ofMillis(workload.setupTimeoutMs().toLong()),
            defaults.maxSendPayloadSize(),
            defaults.maxReceivePayloadSize(),
            defaults.heartbeatEnabled(),
            defaults.heartbeatInterval(),
            defaults.heartbeatTimeout(),
            defaults.reconnectEnabled(),
            defaults.reconnectInitialDelay(),
            defaults.reconnectMaxDelay(),
            defaults.reconnectBackoffFactor(),
            defaults.skipServerCertificateValidation(),
            defaults.compression(),
            defaults.compressionCodec(),
            defaults.nameResolver(),
            defaults.typedCodec(),
        )
        return ZLinkStreamConnectorFactory.create(options)
    }

    override fun prepare(): CompletionStage<Void> = completionStage {
        val total = count()
        val first = first()
        sequences = AtomicLongArray(total)
        val endpoint = URI.create(manifest.roles().first { it.streamEndpoint() != null }.streamEndpoint())
        val evidence = arrayOfNulls<Any>(total)
        val next = AtomicInteger()
        val successes = AtomicInteger()
        val failures = AtomicInteger()
        val lanes = minOf(manifest.workload().connectConcurrency(), maxOf(1, total))
        coroutineScope {
            (0 until lanes).map {
                async(Dispatchers.IO) {
                    while (true) {
                        val local = next.getAndIncrement()
                        if (local >= total) break
                        val id = first + local
                        val started = PerfClock.now()
                        try {
                            val connector = create(endpoint)
                            owned.add(connector)
                            val setupRequest = measurement.request(id, sequences.incrementAndGet(local), true)
                            val wrapper = connector.kotlin()
                            wrapper.connect().await()
                            val reply = wrapper.request<PerfEchoReply>(setupRequest)
                                .timeout(Duration.ofMillis(manifest.workload().setupTimeoutMs().toLong()))
                                .await()
                            PayloadPattern.validateIdentity(setupRequest, reply)
                            measurement.pattern().validate(reply.payload())
                            if (!wrapper.isConnected) error("Connector lost its connection during setup.")
                            connectors.add(Connected(id, connector))
                            successes.incrementAndGet()
                            val observed = LinkedHashMap<String, Any>()
                            observed["clientId"] = id
                            observed["state"] = wrapper.state.toString()
                            observed["isConnected"] = wrapper.isConnected
                            observed["setupLatencyNs"] = DecimalText.of(PerfClock.now() - started)
                            observed["correlationId"] = setupRequest.correlationId()
                            evidence[local] = Evidence.of(
                                "connectorSetupAndTypedProbe",
                                "Kotlin connector wrapper connect().await + request<PerfEchoReply>().await",
                                observed,
                            )
                        } catch (error: Exception) {
                            if (error is CancellationException) throw error
                            failures.incrementAndGet()
                            val cause = Measurement.unwrap(error)
                            val observed = LinkedHashMap<String, Any?>()
                            observed["clientId"] = id
                            observed["message"] = cause.message
                            observed["setupLatencyNs"] = DecimalText.of(PerfClock.now() - started)
                            evidence[local] = Evidence.of("connectorSetupFailure", cause.javaClass.name, observed)
                        }
                    }
                }
            }.awaitAll()
        }
        measurement.connected(successes.get().toLong())
        measurement.connectionFailures(failures.get().toLong())
        measurement.setupEvidence(evidence.toList())
        null
    }

    override fun run(): CompletionStage<Void> = completionStage {
        val first = first()
        coroutineScope {
            connectors.flatMap { entry ->
                (0 until manifest.workload().inflight()).map { slot ->
                    launch(Dispatchers.IO) {
                        while (measurement.canIssue()) {
                            val local = entry.id - first
                            val request = measurement.request(entry.id, sequences.incrementAndGet(local), false)
                            val started = measurement.beginOperation()
                            if (started < 0) break
                            try {

                                    val sent = request.withSentTicks(started)
                                    val reply = entry.connector.kotlin()
                                        .request<PerfEchoReply>(sent)
                                        .timeout(Duration.ofMillis(manifest.workload().requestTimeoutMs().toLong()))
                                        .await()
                                    PayloadPattern.validateIdentity(sent, reply)
                                    measurement.pattern().validate(reply.payload())
                                measurement.completeOperation(started)
                            } catch (error: Exception) {
                                measurement.completeOperation(started, error)
                                if (error is CancellationException) throw error
                            }
                        }
                    }
                }
            }.forEach { it.join() }
        }
        null
    }

    override fun close() {
        kotlinx.coroutines.runBlocking {
            owned.forEach { connector -> connector.kotlin().close().await() }
        }
    }
}
