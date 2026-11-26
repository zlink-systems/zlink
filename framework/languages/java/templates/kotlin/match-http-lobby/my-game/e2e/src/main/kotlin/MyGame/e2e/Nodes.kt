package MyGame.e2e

import MyGame.*
import java.nio.file.Files
import java.nio.file.Path
import java.util.UUID
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

private const val RUN_ROOM_CAPACITY = 1

private const val DOCKER_CLEANUP_SECONDS = 120L

private class Node(
    val id: String,
    module: String,
    config: Path,
    root: Path,
    logs: Path,
    private val foreground: Boolean,
) {
    private val output = LinkedBlockingQueue<String>()
    val process =
        DockerRuntime.process(
                foregroundJavaPrefix(foreground) +
                    listOf(
                        Path.of(
                                System.getProperty("java.home"),
                                "bin",
                                if (isWindows()) "java.exe" else "java",
                            )
                            .toString(),
                        "--enable-native-access=ALL-UNNAMED",
                        "-Dstdout.encoding=UTF-8",
                        "-Dstderr.encoding=UTF-8",
                        "-DMyGame.flowTarget=System.out",
                        "-DMyGame.flowPrefix=FLOW ",
                    ) +
                    foregroundJavaFlags(foreground) +
                    listOf(
                        "-cp",
                        (root.resolve("server/$module/build/install/$module/lib").toString() +
                            "/*"),
                        "MyGame.$module.ProgramKt",
                        config.toString(),
                    ),
                config,
            )
            .directory(root.toFile())
            .redirectError(logs.resolve("$id-stderr.log").toFile())
            .start()
    private val input = process.outputWriter()

    private val stdout = captureNodeOutput(id, process, logs, foreground, output)

    fun waitFor(prefix: String): String {
        val expires =
            System.nanoTime() +
                (if (prefix == Control.SAFE_TO_SHUTDOWN) Control.SAFE_SHUTDOWN_DEADLINE.toNanos()
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
        if (foreground) println("[$id] Stopping")
        command(Control.SHUTDOWN)
        val stopped = waitFor(Control.STOPPED)
        if (DockerRuntime.enabled) {
            check(stopped == Control.STOPPED) { "$id shutdown failed: $stopped" }
            if (!process.waitFor(DOCKER_CLEANUP_SECONDS, TimeUnit.SECONDS)) {
                System.err.println("infra cleanup delay: $id ${DOCKER_CLEANUP_SECONDS}s")
                process.destroyForcibly().waitFor()
                return
            }
        } else if (!process.waitFor(Control.PROCESS_SECONDS, TimeUnit.SECONDS)) {
            DockerRuntime.threadDump(process)
            error("$id failed to exit; thread dump is in $id-stdout.log")
        }
        stdout.join()
        check(process.exitValue() == 0) { "$id shutdown exit: ${process.exitValue()}" }
        if (foreground) println("[$id] Stopped")
    }

    fun cleanup() {
        withCleanup(
            cleanup = {
                if (process.isAlive) {
                    process.destroyForcibly()
                    process.waitFor()
                }
                stdout.join()
            }
        ) {
            stop()
        }
    }
}

fun isWindows() = System.getProperty("os.name").startsWith("Windows")

class Nodes(
    private val root: Path,
    private val foreground: Boolean = false,
    logDirectory: Path? = null,
) : AutoCloseable {
    private val runId = UUID.randomUUID().toString()
    val logs: Path =
        (logDirectory ?: root.resolve(".logs/run-$runId")).also { Files.createDirectories(it) }
    private val project = "my-game-$runId"
    private val nodes = mutableMapOf<String, Node>()
    private val configs = mutableMapOf<String, Path>()
    private val modules = mutableMapOf<String, String>()
    private var composeStarted = false
    var endpoint: String = ""
        private set

    var lobbyEndpoint: String = ""
        private set

    fun target() =
        ScenarioTarget(ServerEndpoints(mapOf("Session" to endpoint), lobbyEndpoint), this)

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
            if (foreground) println("[Redis] Starting")
            composeStarted = true
            compose("up", "-d", "--wait")
            if (foreground) println("[Redis] Ready")
        }
        val redis = if (DockerRuntime.enabled) "redis:6379" else compose("port", "redis", "6379")
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
                    roomCapacity = if (foreground) source.roomCapacity else RUN_ROOM_CAPACITY,
                    redisEndpoint = redis,
                    keyPrefix = "my-game:$runId:",
                    meshEndpoint = "tcp://127.0.0.1:0",
                    streamEndpoint = if (role == Roles.SESSION) "ws://127.0.0.1:0" else null,
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
        if (foreground) println("[$id] Starting")
        val generationLogs =
            logs.resolve("$id-${UUID.randomUUID()}").also { Files.createDirectories(it) }
        val node =
            Node(
                id,
                requireNotNull(modules[id]),
                requireNotNull(configs[id]),
                root,
                generationLogs,
                foreground,
            )
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

    fun replace(id: String) {
        requireNotNull(nodes[id]).stop()
        startNode(id)
    }

    fun restartLobby(): ScenarioTarget {
        requireNotNull(nodes["lobby"]).stop()
        lobbyEndpoint = startNode("lobby")["httpEndpoint"].asText()
        return target()
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
                if (foreground) println("[Redis] Stopped")
            } catch (error: Exception) {
                if (failure == null) failure = error else failure.addSuppressed(error)
            }
        }
        failure?.let { throw it }
    }
}
