package MyGame

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*

class Player(override val context: ZLinkActorContext) : ZLinkSuspendingActor() {
    suspend fun deliver(message: Any, visible: Boolean = true) {
        val sourceSpotId: String? = when (message) {
            is PartyView -> null
            is DungeonView -> message.dungeonId
            else -> null
        }
        if (sourceSpotId != null && (context.spotId().orElse(null) != sourceSpotId || !visible)) return
        context.boundSession().kotlin().send(message).await()
    }

    fun checkIdentity(playerId: String) {
        require(context.actorId() == playerId) { "Player identity mismatch" }
    }

    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> Unit
            is ZLinkActorJoinCompletion.Rejected ->
                context.boundSession().kotlin().send(ActionRejected(Rules.JOIN_REJECTED)).await()
            is ZLinkActorJoinCompletion.Failed -> error("Join failed: ${completion.kind()}")
        }
    }
}

class PlayerFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor = Player(context)
}

class PlayerTransfer : ZLinkActorRelocationAdapter<Player> {
    override fun capture(
        actor: Player,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(byteArrayOf())
    }

    override fun restore(
        actor: Player,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        require(state.isEmpty()) { "Player has no application state" }
        return CompletableFuture.completedFuture(null)
    }
}
