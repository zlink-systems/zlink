package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot

import com.fasterxml.jackson.databind.ObjectMapper
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.ZLinkRelocationCancellation
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter
import systems.zlink.samples.kotlin.bingo.server.play.domain.bingo.BingoRoomSettings
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomState

class BingoRoomRelocationAdapter(
    private val json: ObjectMapper,
) : ZLinkSpotRelocationAdapter<BingoRoomSpot> {
    override fun capture(
        spot: BingoRoomSpot,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        ensureActive(cancellation)
        val state = spot.captureRelocationState()
        return CompletableFuture.completedFuture(
            json.writeValueAsBytes(Payload(state.settings, state.state)),
        )
    }

    override fun restore(
        spot: BingoRoomSpot,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        ensureActive(cancellation)
        val payload = json.readValue(state, Payload::class.java)
        spot.restoreRelocationState(
            BingoRoomSpot.RelocationState(payload.settings, payload.state),
        )
        return CompletableFuture.completedFuture(null)
    }

    private fun ensureActive(cancellation: ZLinkRelocationCancellation) {
        if (cancellation.isCancellationRequested) {
            throw java.util.concurrent.CancellationException(
                "Bingo room relocation was cancelled",
            )
        }
    }

    private data class Payload(
        val settings: BingoRoomSettings,
        val state: BingoRoomState,
    )
}
