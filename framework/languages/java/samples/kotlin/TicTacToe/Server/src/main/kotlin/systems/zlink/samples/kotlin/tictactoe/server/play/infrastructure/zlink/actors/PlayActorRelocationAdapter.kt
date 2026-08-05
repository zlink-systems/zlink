package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors

import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter
import systems.zlink.framework.actors.ZLinkRelocationCancellation
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerInfo

// --8<-- [start:doc-relocation-adapter]
class PlayActorRelocationAdapter : ZLinkActorRelocationAdapter<PlayActor> {
    override fun capture(
        actor: PlayActor,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> = CompletableFuture.completedFuture(
        json.writeValueAsBytes(
            TransferState(
                actor.joinedRoomIdOrNull(),
                actor.playerOrNull(),
                actor.destroyAfterEntrySpotJoin,
                actor.disconnected,
            ),
        ),
    )

    override fun restore(
        actor: PlayActor,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        val transferred = json.readValue(state, TransferState::class.java)
        transferred.player?.let(actor::applyPlayer)
        transferred.roomId?.takeIf(String::isNotBlank)?.let(actor::joinGame)
        if (transferred.destroyAfterEntrySpotJoin) actor.markForDestroyAfterRoomLeave()
        if (transferred.disconnected) actor.markDisconnected()
        return CompletableFuture.completedFuture(null)
    }

    companion object {
        private val json = jacksonObjectMapper()
    }

    data class TransferState(
        val roomId: String?,
        val player: PlayerInfo?,
        val destroyAfterEntrySpotJoin: Boolean,
        val disconnected: Boolean,
    )
}
// --8<-- [end:doc-relocation-adapter]
