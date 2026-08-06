package systems.zlink.e2e.kotlin.pubsub.client.Support

import java.io.File
import java.util.concurrent.TimeUnit

class ServerProcessLauncher(private val options: ClientOptions) {
    fun startSubscriber(
        name: String,
        topics: String,
        httpEndpoint: String,
        handlerDelayMillis: Long? = null,
        includeAllTopics: Boolean = true,
        manualEndpoint: String? = null,
        mixedMode: Boolean = false,
        noStore: Boolean = false,
    ): LaunchedServer {
        val command = mutableListOf(
            required(options.subscriberBin, "subscriber-bin"),
            "--rid",
            name,
            "--topics",
            topics,
            "--include-all",
            includeAllTopics.toString(),
            "--http-endpoint",
            httpEndpoint,
            "--redis-location-endpoint",
            if (noStore) "" else required(options.redisLocationEndpoint, "redis-location-endpoint"),
            "--location-key-prefix",
            if (noStore) "" else required(options.locationKeyPrefix, "location-key-prefix"),
            "--log-dir",
            required(options.logDir, "log-dir"),
        )
        if (manualEndpoint != null) {
            command += listOf("--manual-endpoint", manualEndpoint)
        }
        if (mixedMode) {
            command += listOf("--mixed-mode", "true")
        }
        if (handlerDelayMillis != null) {
            command += listOf("--handler-delay-ms", handlerDelayMillis.toString())
        }
        return start(name, command)
    }

    fun startPublisher(): LaunchedServer =
        startPublisher(
            name = "publisher-restarted",
            publisherEndpoint = options.publisherEndpoint,
            httpEndpoint = options.publisherHttp,
            routingId = "publisher-a",
            channelName = "pubsub.kotlin.events",
            noStore = false,
        )

    fun startPublisher(
        name: String,
        publisherEndpoint: String,
        httpEndpoint: String,
        routingId: String,
        channelName: String = "pubsub.kotlin.events",
        noStore: Boolean = false,
        listenPort: Int? = null,
        advertiseHost: String? = null,
    ): LaunchedServer {
        val command = mutableListOf(
            required(options.publisherBin, "publisher-bin"),
            "--http-endpoint",
            required(httpEndpoint, "publisher2-http"),
            "--rid",
            routingId,
            "--channel-name",
            channelName,
            "--redis-location-endpoint",
            if (noStore) "" else required(options.redisLocationEndpoint, "redis-location-endpoint"),
            "--location-key-prefix",
            if (noStore) "" else required(options.locationKeyPrefix, "location-key-prefix"),
            "--log-dir",
            required(options.logDir, "log-dir"),
        )
        if (listenPort != null) {
            command += listOf("--publisher-port", listenPort.toString())
        } else {
            command += listOf("--publisher-endpoint", required(publisherEndpoint, "publisher2-endpoint"))
        }
        if (!advertiseHost.isNullOrBlank()) {
            command += listOf("--advertise-host", advertiseHost)
        }
        return start(name, command)
    }

    private fun start(name: String, command: List<String>): LaunchedServer {
        val logDir = File(required(options.logDir, "log-dir")).also { it.mkdirs() }
        val process = ProcessBuilder(command)
            .redirectOutput(File(logDir, "$name.stdout.log"))
            .redirectError(File(logDir, "$name.stderr.log"))
            .start()
        return LaunchedServer(process)
    }

    private fun required(value: String, option: String): String =
        value.takeIf { it.isNotBlank() } ?: throw IllegalArgumentException("--$option is required for ${options.mode}.")
}

class LaunchedServer(private val process: Process) : AutoCloseable {
    fun stop() {
        if (!process.isAlive) {
            return
        }
        process.destroy()
        if (!process.waitFor(5, TimeUnit.SECONDS)) {
            process.destroyForcibly()
            process.waitFor(5, TimeUnit.SECONDS)
        }
    }

    override fun close() {
        stop()
    }
}
