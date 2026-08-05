package systems.zlink.e2e.kotlin.registrymessaging.client.Support

import java.io.File
import java.net.ServerSocket
import java.time.Duration

class DynamicClusterLauncher private constructor(
    private val options: ClientOptions,
    private val scenarioName: String,
) : AutoCloseable {
    private val processes = mutableListOf<DynamicProcess>()

    companion object {
        fun start(options: ClientOptions, scenarioName: String): DynamicClusterLauncher {
            return DynamicClusterLauncher(options, scenarioName)
        }

        fun pickEndpoint(): String = "tcp://127.0.0.1:${pickPort()}"

        fun pickHttpUrl(): String = "http://127.0.0.1:${pickPort()}"

        private fun pickPort(): Int =
            ServerSocket(0).use { it.localPort }
    }

    fun startProvider(name: String, rid: String, instanceId: String = rid, weight: Int = 100): DynamicProvider {
        val httpUrl = pickHttpUrl()
        val channelEndpoint = pickEndpoint()
        val process = startProcess(
            name,
            options.providerBin,
            listOf(
                "--rid", rid,
                "--instance-id", instanceId,
                "--http-url", httpUrl,
                "--redis-location-endpoint", options.redisLocationEndpoint,
                "--location-key-prefix", options.locationKeyPrefix,
                "--channel-endpoint", channelEndpoint,
                "--route-endpoint", pickEndpoint(),
                "--weight", weight.toString(),
                "--evidence-file", "${options.logDir}/$name.evidence.log",
                "--log-dir", options.logDir,
            ),
            httpUrl,
            channelEndpoint,
        )
        process.waitReady()
        return DynamicProvider(process, httpUrl, channelEndpoint, rid)
    }

    fun startConsumer(name: String): DynamicConsumer {
        val httpUrl = pickHttpUrl()
        val process = startProcess(
            name,
            options.consumerBin,
            listOf(
                "--http-url", httpUrl,
                "--redis-location-endpoint", options.redisLocationEndpoint,
                "--location-key-prefix", options.locationKeyPrefix,
                "--trace-label", name,
                "--log-dir", options.logDir,
            ),
            httpUrl,
            null,
        )
        process.waitReady()
        return DynamicConsumer(process, httpUrl)
    }

    fun waitPeerEndpoint(consumer: HttpJson, endpoint: String) {
        waitPeers(consumer, "peer endpoint $endpoint") { peers ->
            peers.any { it["nodeRid"] == endpoint }
        }
    }

    fun waitPeerEndpointAbsent(consumer: HttpJson, endpoint: String) {
        waitPeers(consumer, "peer endpoint removal $endpoint") { peers ->
            peers.none { it["nodeRid"] == endpoint }
        }
    }

    fun waitPeerCount(consumer: HttpJson, count: Int) {
        waitPeers(consumer, "peer count $count") { peers -> peers.size == count }
    }

    fun stop(provider: DynamicProvider) {
        provider.process.stop()
        processes.remove(provider.process)
    }

    private fun waitPeers(
        consumer: HttpJson,
        description: String,
        predicate: (List<Map<String, Any?>>) -> Boolean,
    ) {
        val deadline = System.nanoTime() + Duration.ofSeconds(45).toNanos()
        var latest: List<Map<String, Any?>> = emptyList()
        while (System.nanoTime() < deadline) {
            latest = consumer.get("/locations/peers")
            if (predicate(latest)) {
                return
            }
            Thread.sleep(100)
        }
        throw IllegalStateException("Timed out waiting for $description: $latest")
    }

    private fun startProcess(
        name: String,
        binary: String,
        arguments: List<String>,
        httpUrl: String,
        channelEndpoint: String?,
    ): DynamicProcess {
        val process = ProcessBuilder(listOf(binary) + arguments)
            .redirectOutput(File(options.logDir, "$name.stdout.log"))
            .redirectError(File(options.logDir, "$name.stderr.log"))
            .start()
        val dynamic = DynamicProcess(process, httpUrl, channelEndpoint)
        processes += dynamic
        return dynamic
    }

    override fun close() {
        processes.reversed().forEach { it.stop() }
        processes.clear()
    }
}

data class DynamicProvider(
    val process: DynamicProcess,
    val httpUrl: String,
    val channelEndpoint: String,
    val routingId: String,
)

data class DynamicConsumer(
    val process: DynamicProcess,
    val httpUrl: String,
)

class DynamicProcess(
    private val process: Process,
    val httpUrl: String,
    val channelEndpoint: String?,
) {
    fun waitReady() {
        val http = HttpJson(httpUrl)
        val deadline = System.nanoTime() + Duration.ofSeconds(30).toNanos()
        while (System.nanoTime() < deadline) {
            if (!process.isAlive) {
                throw IllegalStateException("Process exited before readiness: ${process.exitValue()}.")
            }
            try {
                http.get<Map<String, Any>>("/health")
                return
            } catch (_: Exception) {
                Thread.sleep(250)
            }
        }
        throw IllegalStateException("Process did not become ready: $httpUrl.")
    }

    fun stop() {
        if (!process.isAlive) {
            return
        }
        try {
            HttpJson(httpUrl).post<Map<String, Any>>("/shutdown")
            process.waitFor(5, java.util.concurrent.TimeUnit.SECONDS)
        } catch (_: Exception) {
        }
        if (process.isAlive) {
            process.destroyForcibly()
        }
    }
}
