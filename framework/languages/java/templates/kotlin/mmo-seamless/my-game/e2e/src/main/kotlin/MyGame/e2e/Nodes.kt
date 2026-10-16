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
    private val process =
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
    private val roles = mutableMapOf<String, String>()
    private var composeStarted = false
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
        composeStarted = true
        compose("up", "-d", "--wait")
        val redis = compose("port", "redis", "6379")
        val structure = Json.mapper.readTree(root.resolve("zlink.structure.json").toFile())
        val startupOrder = listOf("zone", "instance", "session")
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
                    zoneCapacity =
                        if (
                            role == "zone" && Rules.zoneId(Control.REPLACEMENT_X) !in source.zoneIds
                        )
                            Rules.ZONE_COUNT - 1
                        else source.zoneCapacity,
                )
            val path = logs.resolve("$id.json")
            Json.mapper.writeValue(path.toFile(), config)
            configs[id] = path
            roles[id] = role
            val ready = startNode(id)
            if (role == "session") endpoint = ready["streamEndpoint"].asText()
        }
    }

    private fun startNode(id: String): com.fasterxml.jackson.databind.JsonNode {
        val generationLogs =
            logs.resolve("$id-${UUID.randomUUID()}").also { Files.createDirectories(it) }
        val node =
            Node(id, requireNotNull(roles[id]), requireNotNull(configs[id]), root, generationLogs)
        nodes[id] = node
        return Json.mapper.readTree(node.waitFor(Control.READY).removePrefix(Control.READY))
    }

    fun drain(id: String) {
        val node = requireNotNull(nodes[id])
        node.command(Control.VACATE_ZONE)
        node.waitFor(Control.ZONE_VACATED)
        node.command(Control.DRAIN)
        for (stage in
            listOf(Control.RELOCATION_REQUESTED, Control.RELOCATED, Control.SAFE_TO_SHUTDOWN)) node
            .waitFor(stage)
    }

    fun replace(id: String) {
        requireNotNull(nodes[id]).stop()
        val path = requireNotNull(configs[id])
        val config = Json.mapper.readValue(path.toFile(), NodeConfig::class.java)
        Json.mapper.writeValue(path.toFile(), config.copy(bootstrap = false))
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
