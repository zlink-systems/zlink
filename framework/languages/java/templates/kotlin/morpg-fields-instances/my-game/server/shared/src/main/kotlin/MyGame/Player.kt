package MyGame

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*

class Player(override val context: ZLinkActorContext, private val config: NodeConfig) :
    ZLinkSuspendingActor() {
    var position = EnterField()
        private set
    var partyId: String? = null

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
        return CompletableFuture.completedFuture(
            Json.mapper.writeValueAsBytes(PlayerSnapshot(actor.position.x, actor.position.sequence, actor.partyId))
        )
    }

    override fun restore(
        actor: Player,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        val snapshot = Json.mapper.readValue(state, PlayerSnapshot::class.java)
        actor.apply(EnterField(snapshot.x, snapshot.sequence))
        actor.partyId = snapshot.partyId
        return CompletableFuture.completedFuture(null)
    }
}

data class PlayerSnapshot(val x: Double = 0.0, val sequence: Long = 0L, val partyId: String? = null)
