package MyGame.channel.infrastructure.zlink

import MyGame.*
import java.util.concurrent.CancellationException
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.spots.*

private fun ZLinkRelocationCancellation.ensureActive() {
    if (isCancellationRequested) throw CancellationException("Application state transfer cancelled")
}

class PlayerTransfer : ZLinkActorRelocationAdapter<Player> {
    override fun capture(
        actor: Player,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(actor.gold))
    }

    override fun restore(
        actor: Player,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        actor.restore(Json.mapper.readValue(state, Long::class.javaObjectType))
        return CompletableFuture.completedFuture(null)
    }
}

// Framework owns Spot identity and membership; the Zone has no application state.
class ZoneTransfer : ZLinkSpotRelocationAdapter<Zone> {
    override fun capture(
        spot: Zone,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(byteArrayOf())
    }

    override fun restore(
        spot: Zone,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        require(state.isEmpty()) { "Zone has no application state" }
        return CompletableFuture.completedFuture(null)
    }
}
