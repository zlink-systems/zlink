package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors

import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter
import systems.zlink.framework.actors.ZLinkRelocationCancellation

class PlayerActorRelocationAdapter : ZLinkActorRelocationAdapter<PlayerActor> {
    override fun capture(
        actor: PlayerActor,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> = CompletableFuture.completedFuture(
        json.writeValueAsBytes(
            TransferState(
                actor.displayName,
                actor.roomId,
                actor.destroyAfterEntrySpotJoin,
                actor.disconnected,
            ),
        ),
    )

    override fun restore(
        actor: PlayerActor,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        val transferred = json.readValue(state, TransferState::class.java)
        actor.setDisplayName(transferred.displayName)
        if (transferred.roomId.isNotBlank()) actor.joinRoom(transferred.roomId)
        if (transferred.destroyAfterEntrySpotJoin) actor.markForDestroyAfterRoomLeave()
        if (transferred.disconnected) actor.markDisconnected()
        return CompletableFuture.completedFuture(null)
    }

    companion object {
        private val json = jacksonObjectMapper()
    }

    data class TransferState(
        val displayName: String,
        val roomId: String,
        val destroyAfterEntrySpotJoin: Boolean,
        val disconnected: Boolean,
    )
}
