package MyGame.region.infrastructure.zlink

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*

class Player(override val context: ZLinkActorContext, private val actors: ZLinkActorClient) :
    ZLinkSuspendingActor() {
    fun checkIdentity(id: String) {
        require(id == context.actorId()) { "Player identity mismatch" }
    }

    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> Unit
            // The current Entry owns selection after a capacity rejection.
            is ZLinkActorJoinCompletion.Rejected ->
                actors
                    .kotlin()
                    .sendToActor(context.actorId(), completion.reply().decode<RegionFull>())
                    .await()
            is ZLinkActorJoinCompletion.Failed -> error("Region join failed: ${completion.kind()}")
        }
    }
}

class PlayerFactory(private val actors: ZLinkActorClient) : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        Player(context, actors)
}

// Framework owns identity, membership, routes and Session binding.
class PlayerTransfer : ZLinkActorRelocationAdapter<Player> {
    private val empty = byteArrayOf()

    override fun capture(
        actor: Player,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(empty)
    }

    override fun restore(
        actor: Player,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        require(state.isEmpty()) { "Unexpected Player state" }
        return CompletableFuture.completedFuture(null)
    }
}
