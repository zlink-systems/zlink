package MyGame.e2e

import MyGame.Json
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.NoSuchFileException
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.util.concurrent.CountDownLatch
import kotlin.system.exitProcess

data class ServerEndpoints(val streams: Map<String, String>, val http: String? = null)

class ScenarioTarget(val server: ServerEndpoints, val deployment: Nodes? = null) {
    val endpoint
        get() = server.streams.values.single()

    val httpEndpoint
        get() = requireNotNull(server.http)
}

private fun state(root: Path) = root.resolve(".logs/server.json")

private fun openLock(root: Path) =
    FileChannel.open(
        state(root).resolveSibling("server.lock"),
        StandardOpenOption.CREATE,
        StandardOpenOption.WRITE,
    )

internal fun runningServer(root: Path): ServerEndpoints? {
    val state = state(root)
    if (!Files.isRegularFile(state)) return null
    return openLock(root).use { channel ->
        channel.tryLock()?.use {
            return null
        }
        try {
            Json.mapper.readValue(Files.readString(state), ServerEndpoints::class.java)
        } catch (_: NoSuchFileException) {
            null
        }
    }
}

object ServerHost {
    @JvmStatic
    fun main(args: Array<String>) {
        val root = Path.of("").toAbsolutePath().normalize()
        if (args.contentEquals(arrayOf("--is-running"))) {
            exitProcess(if (runningServer(root) != null) 0 else 1)
        }
        require(args.isEmpty()) { "Usage: run-server" }
        checkStructure(root)

        val state = state(root)
        Files.createDirectories(state.parent)
        openLock(root).use { channel ->
            val lock =
                checkNotNull(channel.tryLock()) { "A server is already starting or running." }
            lock.use {
                Files.deleteIfExists(state)
                val temporary = state.resolveSibling("server.json.tmp")
                val nodes = Nodes(root, foreground = true)
                var closed = false
                fun close() =
                    synchronized(nodes) {
                        if (!closed) {
                            closed = true
                            try {
                                Files.deleteIfExists(state)
                            } finally {
                                nodes.close()
                            }
                        }
                    }
                val hook =
                    Thread(
                        {
                            try {
                                close()
                            } catch (failure: Exception) {
                                failure.printStackTrace(System.err)
                            }
                        },
                        "server-shutdown",
                    )
                Runtime.getRuntime().addShutdownHook(hook)
                println("Logs: ${nodes.logs}")
                withCleanup(
                    cleanup = {
                        try {
                            close()
                        } finally {
                            Files.deleteIfExists(temporary)
                        }
                    }
                ) {
                    synchronized(nodes) {
                        check(!closed) { "Server shutdown requested" }
                        nodes.start()
                        val endpoints = nodes.target().server
                        Json.mapper.writeValue(temporary.toFile(), endpoints)
                        Files.move(temporary, state, StandardCopyOption.ATOMIC_MOVE)
                        val clients = endpoints.streams.values.joinToString()
                        println("Server ready: $clients (game clients connect here)")
                        endpoints.http?.let { println("API: $it") }
                    }
                    CountDownLatch(1).await()
                }
            }
        }
    }
}
