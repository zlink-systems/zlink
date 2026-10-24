package MyGame.e2e

import MyGame.*
import java.nio.file.Files
import java.nio.file.Path
import java.util.UUID
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

private const val SESSION_CONFIG = "server/config/session.json"

private class Node(val id: String, config: Path, root: Path, logs: Path) {
    private val output = LinkedBlockingQueue<String>()
    private val process =
        DockerRuntime.process(
                listOf(
                    Path.of(
                            System.getProperty("java.home"),
                            "bin",
                            if (isWindows()) "java.exe" else "java",
                        )
                        .toString(),
                    "--enable-native-access=ALL-UNNAMED",
                    "-cp",
                    (root.resolve("server/$id/build/install/$id/lib").toString() + "/*"),
                    "MyGame.$id.ProgramKt",
                    config.toString(),
                ),
                config,
            )
            .directory(root.toFile())
            .redirectError(logs.resolve("$id-stderr.log").toFile())
            .start()
    private val input = process.outputWriter()

    init {
        thread(name = "$id-output", isDaemon = true) {
            logs.resolve("$id-stdout.log").toFile().bufferedWriter().use { writer ->
                process.inputReader().useLines { lines ->
                    lines.forEach {
                        writer.appendLine(it)
                        writer.flush()
                        output.add(it)
                    }
                }
            }
            output.add(Control.PROCESS_EXIT + process.waitFor())
        }
    }

    fun waitFor(prefix: String): String {
        val expires =
            System.nanoTime() +
                (if (prefix == Control.SAFE_TO_SHUTDOWN) Control.SAFE_DEADLINE.toNanos()
                else TimeUnit.SECONDS.toNanos(Control.PROCESS_SECONDS))
        while (true) {
            val line =
                output.poll((expires - System.nanoTime()).coerceAtLeast(0), TimeUnit.NANOSECONDS)
                    ?: error("$id timed out waiting for $prefix; inspect node stderr")
            if (line.startsWith(Control.PROCESS_EXIT)) error("$id exited before $prefix: $line")
            if (line.startsWith(prefix)) return line
        }
    }

    fun command(command: String) {
        input.write(command + "\n")
        input.flush()
    }

    fun stop() {
        check(process.isAlive) { "$id exited unexpectedly: ${process.exitValue()}" }
        command(Control.SHUTDOWN)
        waitFor(Control.STOPPED)
        check(process.waitFor(Control.PROCESS_SECONDS, TimeUnit.SECONDS))
        check(process.exitValue() == 0) { "$id shutdown exit: ${process.exitValue()}" }
    }

    fun cleanup() {
        withCleanup(
            cleanup = {
                if (process.isAlive) {
                    process.destroyForcibly()
                    process.waitFor()
                }
            }
        ) {
            stop()
        }
    }
}

private fun isWindows() = System.getProperty("os.name").startsWith("Windows")

class Nodes(private val root: Path) : AutoCloseable {
    private val runId = UUID.randomUUID().toString()
    val logs: Path = root.resolve(".logs/run-$runId").also { Files.createDirectories(it) }
    private val project = "my-game-$runId"
    private var composeStarted = false
    private var session: Node? = null
    var endpoint: String = ""
        private set

    private fun compose(vararg args: String): String {
        val process =
            ProcessBuilder(
                    listOf(
                        "docker",
                        "compose",
                        "-p",
                        project,
                        "-f",
                        root.resolve("docker-compose.yml").toString(),
                    ) + args
                )
                .directory(root.toFile())
                .redirectErrorStream(true)
                .start()
        val output = process.inputReader().readText()
        Files.writeString(
            logs.resolve("compose.log"),
            output,
            java.nio.file.StandardOpenOption.CREATE,
            java.nio.file.StandardOpenOption.APPEND,
        )
        check(process.waitFor() == 0) { "Compose failed: $output" }
        return output.trim()
    }

    fun start() {
        if (!DockerRuntime.enabled) {
            composeStarted = true
            compose("up", "-d", "--wait", "redis")
        }
        val redis = if (DockerRuntime.enabled) "redis:6379" else compose("port", "redis", "6379")
        val declared =
            Json.mapper.readValue(root.resolve(SESSION_CONFIG).toFile(), NodeConfig::class.java)
        check(declared.id == Names.SESSION) { "Session configuration Id differs from the node" }
        val config = declared.copy(redisEndpoint = redis, keyPrefix = "my-game:$runId:")
        val path = logs.resolve("${config.id}.json")
        Json.mapper.writeValue(path.toFile(), config)
        val generationLogs =
            logs.resolve("${config.id}-${UUID.randomUUID()}").also { Files.createDirectories(it) }
        val started = Node(config.id, path, root, generationLogs)
        session = started
        val ready = Json.mapper.readTree(started.waitFor(Control.READY).removePrefix(Control.READY))
        endpoint = ready["streamEndpoint"].asText()
    }

    fun drain() {
        val node = requireNotNull(session)
        node.command(Control.DRAIN)
        for (stage in
            listOf(Control.RELOCATION_REQUESTED, Control.RELOCATED, Control.SAFE_TO_SHUTDOWN)) node
            .waitFor(stage)
    }

    override fun close() {
        var failure: Throwable? = null
        try {
            session?.cleanup()
        } catch (error: Exception) {
            failure = error
        }
        if (composeStarted) {
            try {
                compose("down", "--volumes", "--remove-orphans")
            } catch (error: Exception) {
                if (failure == null) failure = error else failure.addSuppressed(error)
            }
        }
        failure?.let { throw it }
    }
}
