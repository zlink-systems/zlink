package MyGame.matchmaker

import MyGame.*
import java.time.Duration
import java.util.UUID
import org.slf4j.LoggerFactory
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.errors.ZLinkOperationCanceledException
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

class Pool(override val context: ZLinkInstanceSpotContext, private val actors: ZLinkActorClient) :
    ZLinkSuspendingInstanceSpot() {
    private val queue = MatchQueue()

    override fun configure() {
        context.handlers().addPacket(PreparedHandler::class.java)
        context.handlers().addPacket(EnterHandler::class.java)
        context.handlers().addPacket(DisconnectHandler::class.java)
    }

    override suspend fun onInitializeSuspending() {
        context
            .addTimer<MatchTick>(
                "match",
                Duration.ofMillis(500),
                ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false),
            )
            .await()
    }

    suspend fun enqueue(request: MatchRequest): MatchQueued {
        val ticket = queue.enqueue(Ticket(request.requestKey, request.playerId, now() + WAIT_NANOS))
        tick()
        return MatchQueued(ticket.id)
    }

    suspend fun tick() {
        val now = now()
        for (assignment in queue.overdue(now)) {
            withdraw(assignment, assignment.entered.orEmpty(), reason = MatchReason.EXPIRED)
        }
        for ((ticket, reason) in queue.expired(now)) {
            notify(ticket, MatchCancelled(ticket.id, reason))
        }
        val assignment =
            queue.reserve(now() + ENTRY_NANOS) { "room-${UUID.randomUUID()}" } ?: return
        val request = PrepareRoom(assignment.id, assignment.tickets.map { it.playerId })
        try {
            context
                .outbound()
                .kotlin()
                .sendToSpot(Names.ROOM_PROVISIONER_KEY, request)
                .instanceSpot(Names.ROOM_PROVISIONER)
                .inMesh(Names.MESH)
                .await()
        } catch (failure: ZLinkOperationCanceledException) {
            throw failure
        } catch (failure: ZLinkFrameworkException) {
            // The reservation still follows its entry deadline.
            logger.warn("Room preparation send failed for {}", assignment.id, failure)
        }
    }

    suspend fun prepared(message: RoomPrepared) {
        val assignment = queue.assigned(message.assignmentId)
        if (assignment?.entered != null) return
        if (assignment == null || now() >= assignment.deadline) {
            if (assignment != null) withdraw(assignment, emptySet(), reason = MatchReason.EXPIRED)
            if (message.ok) sendWithdrawal(message.assignmentId)
            return
        }
        if (!message.ok) {
            queue.finish(assignment, assignment.tickets.map { it.id }.toSet())
            return
        }
        assignment.entered = mutableSetOf()
        for (ticket in assignment.tickets) {
            notify(ticket, MatchAssigned(ticket.id, assignment.id, assignment.id))
        }
    }

    suspend fun entered(report: EnterReport) {
        val assignment = queue.assigned(report.assignmentId) ?: return
        val entered = assignment.entered ?: return
        if (assignment.tickets.none { it.id == report.ticketId }) return
        when {
            now() >= assignment.deadline ->
                withdraw(assignment, entered, reason = MatchReason.EXPIRED)
            !report.ok ->
                withdraw(
                    assignment,
                    assignment.tickets.filter { it.id != report.ticketId }.map { it.id }.toSet(),
                )
            entered.add(report.ticketId) && entered.size == assignment.tickets.size -> {
                queue.finish(assignment)
                context
                    .outbound()
                    .kotlin()
                    .sendToSpot(assignment.id, CommitRoom(assignment.id))
                    .await()
            }
        }
    }

    suspend fun cancel(request: CancelTicket): MatchCancelled {
        if (queue.cancelWaiting(request.ticketId) == null) {
            val assignment =
                checkNotNull(queue.assignmentFor(request.ticketId)) { "Ticket is no longer active" }
            withdraw(
                assignment,
                assignment.tickets.filter { it.id != request.ticketId }.map { it.id }.toSet(),
                replyTicket = request.ticketId,
            )
        }
        return MatchCancelled(request.ticketId, MatchReason.CANCELLED)
    }

    fun disconnected(message: TicketDisconnected) {
        queue.disconnected(message.ticketId, now() + DISCONNECT_NANOS)
    }

    private suspend fun withdraw(
        assignment: Assignment,
        requeue: Set<String>,
        replyTicket: String? = null,
        reason: String = MatchReason.CANCELLED,
    ) {
        queue.finish(assignment, requeue)
        if (assignment.entered != null) sendWithdrawal(assignment.id)
        for (ticket in assignment.tickets) {
            when {
                ticket.id in requeue -> notify(ticket, WithdrawRoom(assignment.id))
                ticket.id != replyTicket -> notify(ticket, MatchCancelled(ticket.id, reason))
            }
        }
    }

    private suspend fun sendWithdrawal(assignmentId: String) {
        context.outbound().kotlin().sendToSpot(assignmentId, WithdrawRoom(assignmentId)).await()
    }

    private suspend fun notify(ticket: Ticket, message: Any) {
        actors.kotlin().sendToActor(ticket.playerId, message).await()
    }

    private companion object {
        val logger = LoggerFactory.getLogger(Pool::class.java)
        val ENTRY_NANOS = Duration.ofSeconds(10).toNanos()
        val DISCONNECT_NANOS = Duration.ofSeconds(15).toNanos()
        val WAIT_NANOS = Duration.ofSeconds(120).toNanos()

        fun now(): Long = System.nanoTime()
    }
}
