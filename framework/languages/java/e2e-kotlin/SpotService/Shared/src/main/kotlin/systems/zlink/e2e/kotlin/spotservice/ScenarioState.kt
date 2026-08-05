package systems.zlink.e2e.kotlin.spotservice

import java.time.Duration

class ScenarioState(private val nodeRid: String) {
    private val entries = mutableListOf<Contracts.EvidenceEntry>()

    fun nodeRid(): String =
        nodeRid

    @Synchronized
    fun record(marker: String, spotRid: String, value: String?) {
        entries.add(Contracts.EvidenceEntry(marker, nodeRid, spotRid, value))
        (this as java.lang.Object).notifyAll()
    }

    @Synchronized
    fun snapshot(): Contracts.EvidenceSnapshot =
        Contracts.EvidenceSnapshot(nodeRid, entries.toList())

    @Synchronized
    fun waitUntilContainsAll(
        expected: List<String>,
        timeout: Duration
    ): Contracts.EvidenceSnapshot {
        val deadline = System.nanoTime() + timeout.toNanos()
        while (true) {
            val current = snapshot()
            if (containsAll(current, expected)) {
                return current
            }

            val remainingNanos = deadline - System.nanoTime()
            if (remainingNanos <= 0) {
                throw IllegalStateException("timed out waiting for evidence: $expected")
            }

            val remainingMillis = (remainingNanos / 1_000_000).coerceAtLeast(1)
            (this as java.lang.Object).wait(remainingMillis)
        }
    }

    private fun containsAll(snapshot: Contracts.EvidenceSnapshot, expected: List<String>): Boolean {
        val entries = snapshot.entries.map { entry ->
            listOfNotNull(entry.marker, entry.nodeRid, entry.spotRid, entry.value).joinToString("|")
        }
        return expected.all { needle -> entries.any { it.contains(needle) } }
    }
}
