package systems.zlink.e2e.kotlin.registrationcodec

import java.util.concurrent.atomic.AtomicInteger

class ScenarioState {
    private val entries = mutableListOf<EvidenceEntry>()
    private val diDisposeCount = AtomicInteger()

    @Synchronized
    fun record(marker: String, packetName: String, value: String) {
        entries.add(EvidenceEntry(marker, packetName, value))
    }

    @Synchronized
    fun snapshot(): EvidenceSnapshot = EvidenceSnapshot(entries.toList())

    fun incrementDiDisposeCount(): Int = diDisposeCount.incrementAndGet()

    fun diDisposeCount(): Int = diDisposeCount.get()
}
