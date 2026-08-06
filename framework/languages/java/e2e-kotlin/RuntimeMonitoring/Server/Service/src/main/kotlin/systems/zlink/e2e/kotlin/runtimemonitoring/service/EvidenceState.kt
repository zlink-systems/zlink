package systems.zlink.e2e.kotlin.runtimemonitoring.service

import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import java.util.List as JavaList

class EvidenceState {
    private val entries = ArrayList<Contracts.EvidenceEntry>()
    private var observer: Contracts.ObserverStatus = Contracts.ObserverStatus(false, 0, false, 0, "")

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

    @Synchronized
    fun observerStatus(): Contracts.ObserverStatus = observer

    @Synchronized
    fun observerStatus(value: Contracts.ObserverStatus) {
        observer = value
    }
}
