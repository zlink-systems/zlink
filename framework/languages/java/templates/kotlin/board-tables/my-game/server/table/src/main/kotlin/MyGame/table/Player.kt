package MyGame.table

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.*
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

internal data class PlayerState(var chips: Long? = null, var match: PlayerMatch? = null)

data class TableAdmission(val tableId: String, val chips: Long)

class Player(
    override val context: ZLinkActorContext,
    private val routes: ZLinkRouteClient,
    private val playerData: PlayerData,
) : ZLinkSuspendingActor() {
    internal var state = PlayerState()

    internal suspend fun loadChips(): Long =
        state.chips ?: playerData.loadPlayer(context.actorId()).also { state.chips = it }

    internal suspend fun disconnected() {
        val ticket = state.match ?: return
        routes.sendPool(TicketDisconnected(ticket.ticketId))
    }

    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        val ticket = state.match
        val assignmentId = ticket?.assignmentId
        if (ticket != null && assignmentId != null) {
            routes.sendPool(
                EnterReport(
                    ticket.ticketId,
                    assignmentId,
                    completion is ZLinkActorJoinCompletion.Accepted,
                )
            )
            return
        }
        when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> Unit
            is ZLinkActorJoinCompletion.Rejected ->
                context.boundSession().kotlin().send(ActionRejected(Rules.TABLE_FULL, 0)).await()
            is ZLinkActorJoinCompletion.Failed -> error("Join failed: ${completion.kind()}")
        }
    }
}

class PlayerFactory(private val routes: ZLinkRouteClient, private val playerData: PlayerData) :
    ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        Player(context, routes, playerData)
}

class PlayerTransfer : ZLinkActorRelocationAdapter<Player> {
    override fun capture(
        actor: Player,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(actor.state))
    }

    override fun restore(
        actor: Player,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        val saved = Json.mapper.readTree(state)
        actor.state =
            when {
                saved == null || saved.isNull -> PlayerState()
                saved.has("ticketId") ->
                    PlayerState(match = Json.mapper.treeToValue(saved, PlayerMatch::class.java))
                else -> Json.mapper.treeToValue(saved, PlayerState::class.java)
            }
        return CompletableFuture.completedFuture(null)
    }
}

class Entry(override val context: ZLinkEntrySpotContext) : ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    override suspend fun onDisconnectActorSuspending(actor: Player) = actor.disconnected()
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
        if (request.reconnect) {
            actor.context().boundSession().kotlin().send(ActionRejected(Rules.NOT_SEATED, 0)).await()
            return
        }
        if (actor.state.match != null) return
        val chips = actor.loadChips()
        val created =
            spots
                .kotlin()
                .getOrCreate(request.tableId, Names.TABLE)
                .inMesh(Names.MESH)
                .request(request)
                .await()
        check(created.state() != ZLinkSpotCreateState.REJECTED) { "Table creation rejected" }
        actor.context().joinSpot(request.tableId, TableAdmission(request.tableId, chips)).defer()
    }
}
