package systems.zlink.e2e.kotlin.runtimemonitoring.service

import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import java.util.List as JavaList

class EvidenceState {
    private val entries = ArrayList<Contracts.EvidenceEntry>()

    @Synchronized
    fun record(
        surface: String,
        sourceName: String,
        event: String,
        detail: String,
    ) {
        entries.add(Contracts.EvidenceEntry(surface, sourceName, event, detail))
    }

    @Synchronized
    fun snapshot(): Contracts.EvidenceSnapshot {
        return Contracts.EvidenceSnapshot(JavaList.copyOf(entries))
    }
}
