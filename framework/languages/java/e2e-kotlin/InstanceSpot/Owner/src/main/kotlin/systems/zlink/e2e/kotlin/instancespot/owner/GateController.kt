package systems.zlink.e2e.kotlin.instancespot.owner

import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CompletionStage

class GateController {
    private val closed = ConcurrentHashMap<String, CompletableFuture<Void>>()

    fun set(gateId: String, open: Boolean) {
        if (open) {
            closed.remove(gateId)?.complete(null)
        } else {
            closed.computeIfAbsent(gateId) { CompletableFuture() }
        }
    }

    fun await(gateId: String): CompletionStage<Void> =
        if (gateId.isBlank()) CompletableFuture.completedFuture(null)
        else closed[gateId] ?: CompletableFuture.completedFuture(null)

    fun awaitPayload(payload: String): CompletionStage<Void> {
        if (!payload.startsWith("gate:")) return CompletableFuture.completedFuture(null)
        val token = payload.removePrefix("gate:")
        return await(token.substringBefore('|'))
    }
}
