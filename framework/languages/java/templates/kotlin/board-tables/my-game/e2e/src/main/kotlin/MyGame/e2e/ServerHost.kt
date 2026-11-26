package MyGame.e2e

import MyGame.Json
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.util.concurrent.CountDownLatch
import kotlin.system.exitProcess

private fun state(root: Path) = root.resolve(".logs/server.json")

private fun openLock(root: Path) =
    FileChannel.open(
        state(root).resolveSibling("server.lock"),
        StandardOpenOption.CREATE,
        StandardOpenOption.WRITE,
    )

internal fun runningEndpoint(root: Path): String? {
    val state = state(root)
    if (!Files.isRegularFile(state)) return null
    openLock(root).use { channel ->
        channel.tryLock()?.use {
            return null
        }
        try {
            return Json.mapper.readTree(Files.readString(state))["sessionEndpoint"].asText()
        } catch (_: java.nio.file.NoSuchFileException) {
            return null
        }
    }
}

object ServerHost {
    @JvmStatic
    fun main(args: Array<String>) {
        val root = Path.of("").toAbsolutePath().normalize()
        if (args.contentEquals(arrayOf("--is-running"))) {
            exitProcess(if (runningEndpoint(root) != null) 0 else 1)
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
                val nodes = Nodes(root, tableCapacity = 0, foreground = true)
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
                try {
                    println("Logs: ${nodes.logs}")
                    synchronized(nodes) {
                        check(!closed) { "Server shutdown requested" }
                        nodes.start()
                        Json.mapper.writeValue(
                            temporary.toFile(),
                            mapOf("sessionEndpoint" to nodes.endpoint),
                        )
                        Files.move(temporary, state, StandardCopyOption.ATOMIC_MOVE)
                        println("Server ready: ${nodes.endpoint} (game clients connect here)")
                    }
                    CountDownLatch(1).await()
                } finally {
                    try {
                        close()
                    } finally {
                        Files.deleteIfExists(temporary)
                    }
                }
            }
        }
    }
}
