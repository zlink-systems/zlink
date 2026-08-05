package systems.zlink.e2e.kotlin.pubsub.subscriber

import kotlinx.coroutines.delay
import systems.zlink.e2e.kotlin.pubsub.shared.EvidenceEntry
import systems.zlink.e2e.kotlin.pubsub.shared.EvidenceSnapshot

class EvidenceStore(
    val subscriberRid: String,
    private val topics: Set<String>,
    private val handlerDelayMillis: Long?,
) {
    private val entries = mutableListOf<EvidenceEntry>()
    private val monitor = Object()

    fun accepts(topic: String): Boolean = topics.contains(topic)

    suspend fun delayIfConfigured(scenario: String) {
        val configuredDelay = handlerDelayMillis
        if (configuredDelay == null || scenario != "ps-b1") {
            return
        }
        delay(configuredDelay)
    }

    fun record(
        marker: String,
        topic: String,
        scenario: String,
        sequence: Int,
        value: String,
    ) {
        synchronized(monitor) {
            entries += EvidenceEntry(marker, subscriberRid, topic, scenario, sequence, value)
            monitor.notifyAll()
        }
    }

    fun snapshot(): EvidenceSnapshot =
        synchronized(monitor) {
            EvidenceSnapshot(subscriberRid, entries.toList())
        }

    fun waitFor(
        marker: String?,
        scenario: String?,
        sequence: Int?,
        valueContains: String?,
        timeoutMillis: Long,
    ): EvidenceSnapshot {
        val deadline = System.nanoTime() + timeoutMillis * 1_000_000L
        synchronized(monitor) {
            while (System.nanoTime() < deadline) {
                if (entries.any { entry ->
                    (marker == null || entry.marker == marker) &&
                        (scenario == null || entry.scenario == scenario) &&
                        (sequence == null || entry.sequence == sequence) &&
                        (valueContains == null || entry.value.contains(valueContains))
                }) {
                    return EvidenceSnapshot(subscriberRid, entries.toList())
                }
                val remainingMillis = ((deadline - System.nanoTime()) / 1_000_000L).coerceAtLeast(1L)
                try {
                    monitor.wait(remainingMillis.coerceAtMost(250L))
                } catch (error: InterruptedException) {
                    Thread.currentThread().interrupt()
                    throw IllegalStateException("interrupted while waiting for subscriber evidence", error)
                }
            }
            throw IllegalStateException("timed out waiting for subscriber evidence")
        }
    }
}
