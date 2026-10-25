package MyGame.channel.infrastructure.zlink

import MyGame.*
import MyGame.channel.application.PlayerRepository
import MyGame.channel.domain.GoldBalance
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*

class Player(
    override val context: ZLinkActorContext,
    private val data: PlayerRepository,
    gold: Long,
) : ZLinkSuspendingActor() {
    private val balance = GoldBalance(gold)
    val gold: Long get() = balance.gold

    fun goldSnapshot() = balance.capture()

    fun restore(snapshot: GoldBalance.Snapshot) {
        balance.restore(snapshot)
    }

    suspend fun earn(): ActionRejected? {
        val decision = balance.previewEarn()
        if (!decision.accepted) return decision.rejection
        data.save(context.actorId(), decision.gold).await()
        balance.commit(decision)
        return null
    }

    fun checkIdentity(id: String) {
        require(context.actorId() == id) { "Player identity mismatch" }
    }

    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        check(completion is ZLinkActorJoinCompletion.Accepted) { "Zone join failed: $completion" }
    }
}

class PlayerFactory(private val data: PlayerRepository) : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor {
        val actor = Player(context, data, data.load(context.actorId()).await())
        println(Control.PLAYER_CREATED + context.actorId())
        return actor
    }
}
