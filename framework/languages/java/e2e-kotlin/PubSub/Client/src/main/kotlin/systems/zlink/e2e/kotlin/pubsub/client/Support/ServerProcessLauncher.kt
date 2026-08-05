package systems.zlink.e2e.kotlin.pubsub.client.Support

import java.io.File
import java.util.concurrent.TimeUnit

class ServerProcessLauncher(private val options: ClientOptions) {
    fun startSubscriber(
        name: String,
        topics: String,
        httpEndpoint: String,
        handlerDelayMillis: Long? = null,
    ): LaunchedServer {
        val command = mutableListOf(
            required(options.subscriberBin, "subscriber-bin"),
            "--rid",
            name,
            "--topics",
            topics,
            "--http-endpoint",
            httpEndpoint,
            "--redis-location-endpoint",
            required(options.redisLocationEndpoint, "redis-location-endpoint"),
            "--location-key-prefix",
            required(options.locationKeyPrefix, "location-key-prefix"),
            "--log-dir",
            required(options.logDir, "log-dir"),
        )
        if (handlerDelayMillis != null) {
            command += listOf("--handler-delay-ms", handlerDelayMillis.toString())
        }
        return start(name, command)
    }

    fun startPublisher(): LaunchedServer {
        val command = listOf(
            required(options.publisherBin, "publisher-bin"),
            "--publisher-endpoint",
            required(options.publisherEndpoint, "publisher-endpoint"),
            "--http-endpoint",
            options.publisherHttp,
            "--redis-location-endpoint",
            required(options.redisLocationEndpoint, "redis-location-endpoint"),
            "--location-key-prefix",
            required(options.locationKeyPrefix, "location-key-prefix"),
            "--log-dir",
            required(options.logDir, "log-dir"),
        )
        return start("publisher-restarted", command)
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
