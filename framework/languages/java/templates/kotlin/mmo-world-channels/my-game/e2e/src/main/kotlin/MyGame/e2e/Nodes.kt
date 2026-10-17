package MyGame.e2e

import MyGame.*
import com.fasterxml.jackson.databind.JsonNode
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import java.util.UUID
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

class Nodes(private val root: Path) : AutoCloseable {
    private val runId = UUID.randomUUID().toString()
    val logs: Path = Files.createDirectories(root.resolve(".logs/run-$runId"))
    private val project = "my-game-$runId"
    private val nodes = linkedMapOf<String, Node>()
    private val configs = mutableMapOf<String, Path>()
    private val modules = mutableMapOf<String, String>()
    private var composeStarted = false
    val endpoints = mutableMapOf<String, String>()
    lateinit var accountsEndpoint: String

    private fun compose(vararg args: String): String {
        val command =
            listOf(
                "docker",
                "compose",
                "-p",
                project,
                "-f",
                root.resolve("docker-compose.yml").toString(),
            ) + args
        val process =
            ProcessBuilder(command).directory(root.toFile()).redirectErrorStream(true).start()
        val output = process.inputReader().use { it.readText() }
        Files.writeString(
            logs.resolve("compose.log"),
            output,
            StandardOpenOption.CREATE,
            StandardOpenOption.APPEND,
        )
        check(process.waitFor() == 0) { "Compose failed: $output" }
        return output.trim()
    }

    fun start() {
        if (!DockerRuntime.enabled) {
            composeStarted = true
            compose("up", "-d", "--wait")
        }
        val redis =
            mapOf(
                Names.WORLD_A to if (DockerRuntime.enabled) "dbA:6379" else compose("port", "dbA", "6379"),
                Names.WORLD_B to if (DockerRuntime.enabled) "dbB:6379" else compose("port", "dbB", "6379"),
            )
        val structure = Json.mapper.readTree(root.resolve("zlink.structure.json").toFile())
        val declarations =
            structure.path("nodes").toList().sortedBy {
                when (it.path("role").asText()) {
                    Roles.CHANNEL -> 0
                    Roles.SESSION -> 1
                    else -> 2
                }
            }
        for (declaration in declarations) {
            val role = declaration.path("role").asText()
            if (role !in Roles.APPLICATION) continue
            val declared =
                Json.mapper.readValue(
                    root.resolve(declaration.path("config").asText()).toFile(),
                    NodeConfig::class.java,
                )
            val id = declared.id
            modules[id] = role
            val worlds =
                if (role == Roles.ACCOUNTS)
                    listOf(
                        WorldInfo(Names.WORLD_A, "World A", endpoints.getValue(Names.WORLD_A)),
                        WorldInfo(Names.WORLD_B, "World B", endpoints.getValue(Names.WORLD_B)),
                    )
                else emptyList()
            val config =
                declared.copy(
                    redisEndpoint = redis[declared.worldId],
                    keyPrefix = "my-game:$runId:${declared.worldId}:",
                    meshEndpoint = "tcp://127.0.0.1:0",
                    streamEndpoint = if (role == Roles.SESSION) "tcp://127.0.0.1:0" else null,
                    worlds = worlds,
                )
            val path = logs.resolve("$id.json")
            Json.mapper.writeValue(path.toFile(), config)
            configs[id] = path
            val ready = startNode(id)
            if (role == Roles.SESSION)
                endpoints[requireNotNull(config.worldId)] = ready.path("streamEndpoint").asText()
            if (role == Roles.ACCOUNTS) accountsEndpoint = ready.path("httpEndpoint").asText()
        }
    }

    private fun startNode(id: String): JsonNode {
        val generationLogs = Files.createDirectories(logs.resolve("$id-${UUID.randomUUID()}"))
        val node = Node(id, modules.getValue(id), configs.getValue(id), root, generationLogs)
        nodes[id] = node
        return Json.mapper.readTree(node.waitFor(Control.READY).removePrefix(Control.READY))
    }

    fun drain(id: String) {
        val node = nodes.getValue(id)
        node.command(Control.DRAIN)
        listOf(Control.RELOCATION_REQUESTED, Control.RELOCATED, Control.SAFE_TO_SHUTDOWN).forEach {
            node.waitFor(it)
        }
    }

    fun createUpdateZone(id: String) {
        val node = nodes.getValue(id)
        node.command(Control.CREATE_UPDATE_ZONE)
        node.waitFor(Control.UPDATE_ZONE_CREATED)
    }

    fun replace(id: String) {
        nodes.getValue(id).stop()
        startNode(id)
    }

    override fun close() {
        var failure: Exception? = null
        fun cleanup(action: () -> Unit) {
            try {
                action()
            } catch (error: Exception) {
                if (failure == null) failure = error else failure!!.addSuppressed(error)
            }
        }
        nodes.values.toList().asReversed().forEach { node -> cleanup { node.cleanup() } }
        if (composeStarted) cleanup { compose("down", "--volumes", "--remove-orphans") }
        failure?.let { throw it }
    }
}

private class Node(private val id: String, module: String, config: Path, root: Path, logs: Path) {
    private val windows = System.getProperty("os.name").startsWith("Windows")
    private val output = LinkedBlockingQueue<String>()
    private val process =
        DockerRuntime.process(
                listOf(
                    Path.of(System.getProperty("java.home"), "bin", if (windows) "java.exe" else "java")
                        .toString(),
                    "--enable-native-access=ALL-UNNAMED",
                    "-cp",
                    root.resolve("server/$module/build/install/$module/lib").toString() + "/*",
                    "MyGame.$module.ProgramKt",
                    config.toString(),
                ),
                config,
            )
            .directory(root.toFile())
            .redirectError(logs.resolve("$id-stderr.log").toFile())
            .start()
    private val input = process.outputWriter()

    init {
        Thread.ofPlatform().daemon().name("$id-output").start {
            try {
                Files.newBufferedWriter(logs.resolve("$id-stdout.log")).use { writer ->
                    process.inputReader().use { reader ->
                        while (true) {
                            val line = reader.readLine() ?: break
                            writer.write(line)
                            writer.newLine()
                            writer.flush()
                            output.add(line)
                        }
                    }
                }
                output.add(Control.PROCESS_EXIT + process.waitFor())
            } catch (error: Exception) {
                output.add(Control.PROCESS_EXIT + error)
            }
        }
    }

    fun waitFor(prefix: String): String {
        val expires =
            System.nanoTime() +
                if (prefix == Control.SAFE_TO_SHUTDOWN) Control.SAFE_DEADLINE.toNanos()
                else TimeUnit.SECONDS.toNanos(Control.PROCESS_SECONDS)
        while (true) {
            val line =
                output.poll((expires - System.nanoTime()).coerceAtLeast(0), TimeUnit.NANOSECONDS)
                    ?: error("$id timed out waiting for $prefix; inspect node stderr")
            check(!line.startsWith(Control.PROCESS_EXIT)) { "$id exited before $prefix: $line" }
            if (line.startsWith(prefix)) return line
        }
    }

    fun command(command: String) {
        input.write(command)
        input.newLine()
        input.flush()
    }

    fun stop() {
        check(process.isAlive) { "$id exited unexpectedly" }
        command(Control.SHUTDOWN)
        waitFor(Control.STOPPED)
        check(process.waitFor(Control.PROCESS_SECONDS, TimeUnit.SECONDS)) { "$id failed to exit" }
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
