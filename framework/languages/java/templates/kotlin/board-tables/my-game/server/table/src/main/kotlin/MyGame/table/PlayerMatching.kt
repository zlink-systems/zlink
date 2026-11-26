package MyGame.table

import MyGame.*
import java.util.UUID
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*

internal data class PlayerMatch(
    val ticketId: String,
    val assignmentId: String? = null,
    val withdrawnAssignments: Set<String> = emptySet(),
)

data class RoomAdmission(val assignmentId: String, val chips: Long)

@ZLinkHandlerGroup(Names.PLAYER)
class PlayerMatching(private val routes: ZLinkRouteClient) {
    @ZLinkSpotActorSend
    suspend fun find(actor: Player, request: FindMatch) {
        val queued =
            actor.state.match?.let { MatchQueued(it.ticketId) }
                ?: run {
                    routes
                        .requestPool<MatchQueued>(
                            MatchRequest(UUID.randomUUID().toString(), actor.context.actorId())
                        )
                        .also { actor.state.match = PlayerMatch(it.ticketId) }
                }
        actor.context.boundSession().kotlin().send(queued).await()
    }

    @ZLinkSpotActorSend
    suspend fun cancel(actor: Player, request: CancelMatch) {
        if (actor.state.match?.ticketId != request.ticketId) return
        val result = routes.requestPool<MatchCancelled>(CancelTicket(request.ticketId))
        cancelled(actor, result)
    }

    @ZLinkSpotActorSend
    suspend fun assigned(actor: Player, message: MatchAssigned) {
        val ticket = actor.state.match ?: return
        if (
            ticket.ticketId != message.ticketId ||
                ticket.assignmentId != null ||
                message.assignmentId in ticket.withdrawnAssignments
        )
            return
        val chips = actor.loadChips()
        actor.state.match = ticket.copy(assignmentId = message.assignmentId)
        actor.context.joinSpot(message.roomId, RoomAdmission(message.assignmentId, chips)).defer()
    }

    @ZLinkSpotActorSend
    suspend fun cancelled(actor: Player, message: MatchCancelled) {
        val ticket = actor.state.match ?: return
        if (ticket.ticketId != message.ticketId) return
        actor.state.match = null
        if (message.reason != MatchReason.DISCONNECTED) {
            actor.context.boundSession().kotlin().send(message).await()
        }
        if (
            ticket.assignmentId != null &&
                actor.context.spotId().orElse(null) == ticket.assignmentId
        ) {
            actor.context.joinEntrySpot().defer()
        }
    }

    @ZLinkSpotActorSend
    suspend fun withdrawn(actor: Player, message: WithdrawRoom) {
        val ticket = actor.state.match ?: return
        if (ticket.assignmentId != message.assignmentId) return
        actor.state.match =
            ticket.copy(
                assignmentId = null,
                withdrawnAssignments = ticket.withdrawnAssignments + message.assignmentId,
            )
        if (actor.context.spotId().orElse(null) == message.assignmentId) {
            actor.context.joinEntrySpot().defer()
        }
    }

    @ZLinkSpotActorSend
    suspend fun committed(
        table: Table,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: CommitRoom,
    ) {
        val ticket = actor.state.match ?: return
        if (ticket.assignmentId != message.assignmentId) return
        val view = table.committedView(actor.context.actorId(), message.assignmentId) ?: return
        actor.state.match = null
        val session = actor.context.boundSession().kotlin()
        session.send(MatchFound(ticket.ticketId, view.tableId, view.yourSeat)).await()
        session.send(view).await()
    }
}

internal suspend inline fun <reified Reply : Any> ZLinkRouteClient.requestPool(
    message: Any
): Reply =
    kotlin()
        .requestToSpot<Reply>(Names.POOL_KEY, message)
        .instanceSpot(Names.POOL)
        .inMesh(Names.MESH)
        .await()

internal suspend fun ZLinkRouteClient.sendPool(message: Any) {
    kotlin().sendToSpot(Names.POOL_KEY, message).instanceSpot(Names.POOL).inMesh(Names.MESH).await()
}
