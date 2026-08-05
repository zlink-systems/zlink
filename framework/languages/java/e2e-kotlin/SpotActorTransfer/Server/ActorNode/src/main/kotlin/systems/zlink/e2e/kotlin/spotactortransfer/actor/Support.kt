package systems.zlink.e2e.kotlin.spotactortransfer.actor

import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList
import systems.zlink.e2e.spotactortransfer.shared.Contracts

class EvidenceStore(
    val nodeRid: String,
    file: String,
) {
    private val file = Path.of(file)
    private val entries = CopyOnWriteArrayList<Contracts.Evidence>()

    init {
        Files.createDirectories(this.file.parent)
    }

    fun add(scenario: String?, actorId: String?, kind: String, value: String?) {
        val evidence = Contracts.Evidence(
            scenario ?: "",
            actorId ?: "",
            kind,
            value ?: "",
            nodeRid,
            "",
            "",
            "",
        )
        entries += evidence
        Files.writeString(
            file,
            evidence.text() + System.lineSeparator(),
            StandardOpenOption.CREATE,
            StandardOpenOption.APPEND,
        )
    }

    fun snapshot(): Contracts.EvidenceSnapshot = Contracts.EvidenceSnapshot(entries.toList())
}

class GateStore {
    private val gates = ConcurrentHashMap<String, CompletableFuture<Void>>()

    fun waitFor(key: String): CompletableFuture<Void> =
        gates.computeIfAbsent(key) { CompletableFuture() }

    fun release(key: String): Boolean =
        gates.computeIfAbsent(key) { CompletableFuture() }.complete(null)
}

class DomainStateStore(directory: String) {
    private val directory = Path.of(directory)

    init {
        Files.createDirectories(this.directory)
    }

    fun save(actorId: String, stateVersion: Int) {
        Files.writeString(path(actorId), stateVersion.toString())
    }

    fun load(actorId: String): Int =
        path(actorId).takeIf(Files::exists)?.let { Files.readString(it).trim().toInt() } ?: 0

    private fun path(actorId: String): Path =
        directory.resolve(actorId.replace(Regex("[^A-Za-z0-9_.-]"), "_") + ".state")
}
