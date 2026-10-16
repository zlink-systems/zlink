package MyGame.e2e

import MyGame.*
import java.nio.file.Files
import java.nio.file.Path
import java.util.UUID
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

private class Node(val id: String, module: String, config: Path, root: Path, logs: Path) {
    private val output = LinkedBlockingQueue<String>()
    val process =
        ProcessBuilder(
                Path.of(
                        System.getProperty("java.home"),
                        "bin",
                        if (isWindows()) "java.exe" else "java",
                    )
                    .toString(),
                "--enable-native-access=ALL-UNNAMED",
                "-cp",
                (root.resolve("server/$module/build/install/$module/lib").toString() + "/*"),
                "MyGame.$module.ProgramKt",
                config.toString(),
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
        val expires = System.nanoTime() + TimeUnit.SECONDS.toNanos(Control.PROCESS_SECONDS)
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
        try {
            stop()
        } finally {
            if (process.isAlive) {
                process.destroyForcibly()
                process.waitFor()
            }
        }
    }
}

fun isWindows() = System.getProperty("os.name").startsWith("Windows")

class Nodes(private val root: Path) : AutoCloseable {
    private val runId = UUID.randomUUID().toString()
    val logs: Path = root.resolve(".logs/run-$runId").also { Files.createDirectories(it) }
    private val project = "my-game-$runId"
    private val nodes = mutableMapOf<String, Node>()
    private val configs = mutableMapOf<String, Path>()
    private val modules = mutableMapOf<String, String>()
    private var composeStarted = false
    var endpoint: String = ""
        private set

    var lobbyEndpoint: String = ""
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
        composeStarted = true
        compose("up", "-d", "--wait")
        val redis = compose("port", "redis", "6379")
        val structure = Json.mapper.readTree(root.resolve("zlink.structure.json").toFile())
        val startupOrder = listOf(Roles.MATCH, Roles.SESSION, Roles.LOBBY)
        val applicationNodes =
            structure["nodes"]
                .filter { it["role"].asText() in startupOrder }
                .sortedBy { startupOrder.indexOf(it["role"].asText()) }
        for (node in applicationNodes) {
            val id = node["id"].asText()
            val role = node["role"].asText()
            val source =
                Json.mapper.readValue(
                    root.resolve(node["config"].asText()).toFile(),
                    NodeConfig::class.java,
                )
            val config =
                source.copy(
                    redisEndpoint = redis,
                    keyPrefix = "my-game:$runId:",
                    meshEndpoint = "tcp://127.0.0.1:0",
                    streamEndpoint = if (role == Roles.SESSION) "tcp://127.0.0.1:0" else null,
                    httpEndpoint = if (role == Roles.LOBBY) "http://127.0.0.1:0" else null,
                    sessionEndpoint = if (role == Roles.LOBBY) endpoint else null,
                )
            val path = logs.resolve("$id.json")
            Json.mapper.writeValue(path.toFile(), config)
            configs[id] = path
            modules[id] = if (role == Roles.LOBBY) "lobby" else role
            val ready = startNode(id)
            if (role == Roles.SESSION) endpoint = ready["streamEndpoint"].asText()
            if (role == Roles.LOBBY) lobbyEndpoint = ready["httpEndpoint"].asText()
        }
    }

    private fun startNode(id: String): com.fasterxml.jackson.databind.JsonNode {
        val generationLogs =
            logs.resolve("$id-${UUID.randomUUID()}").also { Files.createDirectories(it) }
        val node =
            Node(id, requireNotNull(modules[id]), requireNotNull(configs[id]), root, generationLogs)
        nodes[id] = node
        return Json.mapper.readTree(node.waitFor(Control.READY).removePrefix(Control.READY))
    }

    fun drain(id: String) {
        val node = requireNotNull(nodes[id])
        node.command(Control.DRAIN)
        for (stage in
            listOf(Control.RELOCATION_REQUESTED, Control.RELOCATED, Control.SAFE_TO_SHUTDOWN)) node
            .waitFor(stage)
    }

    fun isLiveMatch(id: String): Boolean =
        modules[id] == Roles.MATCH && nodes[id]?.process?.isAlive == true

    fun replace(id: String) {
        requireNotNull(nodes[id]).stop()
        startNode(id)
    }

    override fun close() {
        var failure: Throwable? = null
        for (node in nodes.values.toList().asReversed()) {
            try {
                node.cleanup()
            } catch (error: Exception) {
                if (failure == null) failure = error else failure.addSuppressed(error)
            }
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
