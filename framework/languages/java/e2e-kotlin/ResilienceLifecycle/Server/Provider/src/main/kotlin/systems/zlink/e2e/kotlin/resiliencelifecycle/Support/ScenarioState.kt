package systems.zlink.e2e.kotlin.resiliencelifecycle

import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.future.await
import kotlinx.coroutines.withTimeout

class ScenarioState(private val providerRid: String) {
    private val entries = mutableListOf<Contracts.EvidenceEntry>()
    private val slowRelease = CompletableFuture<Void>()
    private var weight = 100
    private var grayFailure = false
    private var observerThrows = false

    fun providerRid(): String = providerRid

    @Synchronized
    fun weight(value: Int) {
        weight = value
    }

    @Synchronized
    fun weight(): Int = weight

    @Synchronized
    fun grayFailure(value: Boolean) {
        grayFailure = value
    }

    @Synchronized
    fun grayFailure(): Boolean = grayFailure

    @Synchronized
    fun observerThrows(value: Boolean) {
        observerThrows = value
    }

    @Synchronized
    fun observerThrows(): Boolean = observerThrows

    fun releaseSlow() {
        slowRelease.complete(null)
    }

    suspend fun awaitSlowRelease() {
        withTimeout(20_000) { slowRelease.await() }
    }

    @Synchronized
    fun record(marker: String, value: String) {
        entries.add(Contracts.EvidenceEntry(marker, providerRid, value))
    }

    @Synchronized
    fun snapshot(): Contracts.EvidenceSnapshot =
        Contracts.EvidenceSnapshot(providerRid, weight, entries.toList())
}
