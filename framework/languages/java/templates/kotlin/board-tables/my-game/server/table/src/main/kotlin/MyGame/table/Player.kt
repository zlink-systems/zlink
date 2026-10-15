package MyGame.table

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
            is ZLinkActorJoinCompletion.Rejected ->
                context.boundSession().kotlin().send(ActionRejected(Rules.TABLE_FULL)).await()
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
        require(state.isEmpty())
        return CompletableFuture.completedFuture(null)
    }
}

class Entry(override val context: ZLinkEntrySpotContext) : ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}
}

@ZLinkHandlerGroup(Names.PLAYER)
class Join(private val spots: ZLinkSpotManager) {
    @ZLinkSpotActorSend
    suspend fun join(
        entry: Entry,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinTableRequest,
    ) {
        require(actor.context().actorId() == request.playerId)
        val created =
            spots
                .kotlin()
                .getOrCreate(request.tableId, Names.TABLE)
                .inMesh(Names.MESH)
                .request(request)
                .await()
        check(created.state() != ZLinkSpotCreateState.REJECTED) { "Table creation rejected" }
        actor.context().joinSpot(request.tableId, request).defer()
    }
}
