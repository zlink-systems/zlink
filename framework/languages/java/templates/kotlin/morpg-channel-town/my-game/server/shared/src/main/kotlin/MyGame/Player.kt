package MyGame

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*

class Player(override val context: ZLinkActorContext) : ZLinkSuspendingActor() {
    suspend fun deliver(sourceSpotId: String, message: Any) {
        if (context.spotId().orElse(null) != sourceSpotId) return
        context.boundSession().kotlin().send(message).await()
    }

    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> Unit
            is ZLinkActorJoinCompletion.Rejected ->
                context
                    .boundSession()
                    .kotlin()
                    .send(ActionRejected(Rejection.JOIN_REJECTED))
                    .await()
            is ZLinkActorJoinCompletion.Failed -> error("Spot join failed: ${completion.kind()}")
        }
    }
}

class PlayerFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor = Player(context)
}

// Actor identity and Session binding are Framework-owned.
class PlayerTransfer : ZLinkActorRelocationAdapter<Player> {
    override fun capture(
        actor: Player,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(ByteArray(0))
    }

    override fun restore(
        actor: Player,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        require(state.isEmpty()) { "Unexpected Player application state" }
        return CompletableFuture.completedFuture(null)
    }
}
