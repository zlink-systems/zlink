package MyGame.matchmaker

import MyGame.*
import java.time.Duration
import java.util.UUID
import java.util.concurrent.TimeoutException
import org.slf4j.LoggerFactory
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.errors.ZLinkOperationCanceledException
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

class Pool(
    override val context: ZLinkInstanceSpotContext,
    private val spots: ZLinkSpotManager,
    private val routes: ZLinkRouteClient,
    private val actors: ZLinkActorClient,
) : ZLinkSuspendingInstanceSpot() {
    private val queue = MatchQueue()

    override fun configure() {
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
            withdraw(assignment, assignment.entered, reason = MatchReason.EXPIRED)
        }
        for ((ticket, reason) in queue.expired(now)) {
            notify(ticket, MatchCancelled(ticket.id, reason))
        }
        val assignment = queue.reserve(now + ENTRY_NANOS) { "room-${UUID.randomUUID()}" } ?: return
        if (!prepare(assignment)) {
            withdraw(assignment, assignment.tickets.map { it.id }.toSet(), notifyActors = false)
            return
        }
        for (ticket in assignment.tickets) {
            notify(ticket, MatchAssigned(ticket.id, assignment.id, assignment.id))
        }
    }

    private suspend fun prepare(assignment: Assignment): Boolean =
        try {
            val request = PrepareRoom(assignment.id, assignment.tickets.map { it.playerId })
            val created =
                spots.kotlin().getOrCreate(assignment.id, Names.TABLE).inMesh(Names.MESH).await()
            if (created.state() == ZLinkSpotCreateState.REJECTED) false
            else {
                val prepared =
                    routes
                        .kotlin()
                        .requestToSpot<RoomPrepared>(assignment.id, request)
                        .inMesh(Names.MESH)
                        .await()
                prepared.ok && prepared.assignmentId == assignment.id
            }
        } catch (failure: ZLinkOperationCanceledException) {
            throw failure
        } catch (failure: ZLinkFrameworkException) {
            logger.warn("Room preparation failed for {}", assignment.id, failure)
            false
        } catch (failure: TimeoutException) {
            logger.warn("Room preparation timed out for {}", assignment.id, failure)
            false
        }

    suspend fun entered(report: EnterReport) {
        val assignment = queue.assigned(report.assignmentId) ?: return
        if (assignment.tickets.none { it.id == report.ticketId }) return
        when {
            now() >= assignment.deadline ->
                withdraw(assignment, assignment.entered, reason = MatchReason.EXPIRED)
            !report.ok ->
                withdraw(
                    assignment,
                    assignment.tickets.filter { it.id != report.ticketId }.map { it.id }.toSet(),
                )
            assignment.entered.add(report.ticketId) &&
                assignment.entered.size == assignment.tickets.size -> {
                queue.finish(assignment)
                routes.kotlin().sendToSpot(assignment.id, CommitRoom(assignment.id)).await()
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
        notifyActors: Boolean = true,
        reason: String = MatchReason.CANCELLED,
    ) {
        queue.finish(assignment, requeue)
        if (spots.find(assignment.id).await().isPresent) {
            routes.kotlin().sendToSpot(assignment.id, WithdrawRoom(assignment.id)).await()
        }
        if (!notifyActors) return
        for (ticket in assignment.tickets) {
            when {
                ticket.id in requeue -> notify(ticket, WithdrawRoom(assignment.id))
                ticket.id != replyTicket -> notify(ticket, MatchCancelled(ticket.id, reason))
            }
        }
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
