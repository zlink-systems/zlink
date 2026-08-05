package systems.zlink.e2e.kotlin.registrymessaging.provider.Infrastructure

import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import java.time.Duration
import java.time.Instant
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.Semaphore

class EvidenceStore(
    val rid: String,
    val instanceId: String,
    private val filePath: String?,
) {
    private val entries = ConcurrentLinkedQueue<String>()
    private val signal = Semaphore(0)
    private val fileLock = Any()

    init {
        if (!filePath.isNullOrBlank()) {
            val path = Path.of(filePath)
            Files.createDirectories(path.parent)
            Files.writeString(path, "")
        }
    }

    fun add(entry: String) {
        entries.add(entry)
        signal.release()
        if (!filePath.isNullOrBlank()) {
            synchronized(fileLock) {
                Files.writeString(
                    Path.of(filePath),
                    entry + System.lineSeparator(),
                    StandardOpenOption.CREATE,
                    StandardOpenOption.APPEND,
                )
            }
        }
    }

    fun snapshot(): List<String> = entries.toList()

    fun clear() {
        entries.clear()
        if (!filePath.isNullOrBlank()) {
            synchronized(fileLock) {
                Files.writeString(Path.of(filePath), "")
            }
        }
    }

    fun waitUntil(contains: String, timeout: Duration): List<String> {
        val deadline = Instant.now().plus(timeout)
        while (Instant.now().isBefore(deadline)) {
            val current = snapshot()
            if (current.any { it.contains(contains) }) {
                return current
            }
            signal.tryAcquire(100, java.util.concurrent.TimeUnit.MILLISECONDS)
        }
        return snapshot()
    }
}
