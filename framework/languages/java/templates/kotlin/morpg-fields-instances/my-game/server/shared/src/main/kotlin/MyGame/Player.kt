package MyGame

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*

class Player(override val context: ZLinkActorContext, private val config: NodeConfig) :
    ZLinkSuspendingActor() {
    var position = EnterField()
        private set

    fun apply(position: EnterField) {
        this.position = position
    }

    fun checkIdentity(playerId: String) {
        require(context.actorId() == playerId) { "Player identity mismatch" }
    }

    fun move(position: EnterField) {
        context.joinSpot(Rules.fieldId(position.x), position).defer()
    }

    suspend fun sendState() {
        context
            .boundSession()
            .kotlin()
            .send(WorldState(Rules.fieldId(position.x), config.id, position.x, position.sequence))
            .await()
    }

    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        check(completion is ZLinkActorJoinCompletion.Accepted) { "Field join failed: $completion" }
        if (context.spotId().orElse(null) == Rules.fieldId(position.x)) sendState()
    }
}

class PlayerFactory(private val config: NodeConfig) : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        Player(context, config)
}

class PlayerTransfer : ZLinkActorRelocationAdapter<Player> {
    override fun capture(
        actor: Player,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(actor.position))
    }

    override fun restore(
        actor: Player,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        actor.apply(Json.mapper.readValue(state, EnterField::class.java))
        return CompletableFuture.completedFuture(null)
    }
}
