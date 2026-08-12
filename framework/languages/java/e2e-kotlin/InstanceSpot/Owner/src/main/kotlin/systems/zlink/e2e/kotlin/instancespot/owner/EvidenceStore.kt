package systems.zlink.e2e.kotlin.instancespot.owner

import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import java.time.Duration
import java.util.concurrent.atomic.AtomicLong
import systems.zlink.e2e.kotlin.instancespot.shared.Contracts
import systems.zlink.e2e.kotlin.instancespot.shared.Wait

class EvidenceStore(private val options: OwnerOptions) {
    private val sequence = AtomicLong()
    private val events = mutableListOf<Contracts.EvidenceEntry>()

    @Synchronized
    fun record(
        kind: String,
        spotId: String = "",
        operationId: String = "",
        payload: String = "",
        generation: Long = 0,
        activeHandlers: Int = 0,
        detail: String = "",
    ): Contracts.EvidenceEntry {
        val event = Contracts.EvidenceEntry(
            sequence.incrementAndGet(), kind, spotId, operationId, payload,
            options.rid, options.lifecycleId, generation, activeHandlers, detail,
        )
        events += event
        append(event)
        return event
    }

    @Synchronized
    fun snapshot(): Contracts.EvidenceSnapshot =
        Contracts.EvidenceSnapshot(options.rid, options.lifecycleId, events.toList())

    fun waitFor(request: Contracts.EvidenceWaitReq): Contracts.EvidenceWaitRes {
        val snapshot = Wait.until(
            Duration.ofMillis(request.timeoutMilliseconds),
            "timed out waiting for Instance Spot evidence kind=${request.kind} operation=${request.operationId}",
        ) {
            snapshot().takeIf { current ->
                current.events.any { event ->
                    (request.kind.isBlank() || event.kind == request.kind) &&
                        (request.operationId.isBlank() || event.operationId == request.operationId)
                }
            }
        }
        return Contracts.EvidenceWaitRes(true, snapshot)
    }

    private fun append(event: Contracts.EvidenceEntry) {
        if (options.evidenceFile.isBlank()) return
        val path = Path.of(options.evidenceFile)
        path.parent?.let(Files::createDirectories)
        val line = listOf(
            event.sequence, event.kind, event.spotId, event.operationId,
            event.payload, event.objectGeneration, event.activeHandlers,
        ).joinToString("|") + System.lineSeparator()
        Files.writeString(
            path, line, StandardCharsets.UTF_8,
            StandardOpenOption.CREATE, StandardOpenOption.APPEND,
        )
    }
}
