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
    var gold: Long = 0
        private set

    init {
        restore(gold)
    }

    fun restore(value: Long) {
        gold = GoldBalance.validate(value)
    }

    suspend fun earn(amount: Long) {
        val next = GoldBalance.earn(gold, amount)
        data.save(context.actorId(), next).await()
        gold = next
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
