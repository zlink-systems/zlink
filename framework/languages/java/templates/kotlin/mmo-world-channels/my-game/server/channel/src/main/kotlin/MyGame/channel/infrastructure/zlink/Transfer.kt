package MyGame.channel.infrastructure.zlink

import MyGame.*
import MyGame.channel.domain.GoldBalance
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
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(actor.goldSnapshot()))
    }

    override fun restore(
        actor: Player,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        actor.restore(Json.mapper.readValue(state, GoldBalance.Snapshot::class.java))
        return CompletableFuture.completedFuture(null)
    }
}

class ZoneTransfer : ZLinkSpotRelocationAdapter<Zone> {
    override fun capture(
        spot: Zone,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(spot.snapshot()))
    }

    override fun restore(
        spot: Zone,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.restore(Json.mapper.readValue(state, Array<String>::class.java).toList())
        return CompletableFuture.completedFuture(null)
    }
}
