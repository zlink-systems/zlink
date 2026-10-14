package MyGame.match.infrastructure.zlink

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
                context
                    .boundSession()
                    .kotlin()
                    .send(completion.reply().decode<ActionRejected>())
                    .await()
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

class Entry(override val context: ZLinkEntrySpotContext) : ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}
}

@ZLinkHandlerGroup(Names.PLAYER)
class Join {
    // The HTTP lobby creates the Room; Entry only defers the join.
    @ZLinkSpotActorSend
    suspend fun join(
        entry: Entry,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinRoomRequest,
    ) {
        require(actor.context.actorId() == request.playerId) {
            "Player identity differs from Actor"
        }
        actor.context.joinSpot(request.roomId, request).defer()
    }
}
