package MyGame.match

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.*
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

class Player(override val context: ZLinkActorContext) : ZLinkSuspendingActor() {
    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> Unit
            is ZLinkActorJoinCompletion.Rejected -> context.boundSession().kotlin()
                .send(ActionRejected(Rejection.ADMISSION_REJECTED)).await()
            is ZLinkActorJoinCompletion.Failed -> error("Spot join failed: ${completion.kind()}")
        }
    }
}
class PlayerFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor = Player(context)
}
class Entry(override val context: ZLinkEntrySpotContext) : ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}
    override suspend fun onLeaveActorSuspending(actor: Player) {}
}
@ZLinkHandlerGroup(Names.PLAYER)
class EnterQueue {
    @ZLinkSpotActorSend
    suspend fun queue(entry: Entry, actor: Player, dispatch: ZLinkMessageContext, request: QueueRequest) {
        require(actor.context.actorId() == request.playerId) { "Queue identity mismatch" }
        actor.context.joinSpot(Names.MATCHMAKING_ID, request).defer()
    }
}

// Identity and binding are Framework-owned; Player has no application state.
class PlayerTransfer : ZLinkActorRelocationAdapter<Player> {
    override fun capture(actor: Player, cancellation: ZLinkRelocationCancellation): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(ByteArray(0))
    }
    override fun restore(actor: Player, state: ByteArray, cancellation: ZLinkRelocationCancellation): CompletionStage<Void> {
        cancellation.ensureActive()
        require(state.isEmpty()) { "Unexpected Player application state" }
        return CompletableFuture.completedFuture(null)
    }
}
