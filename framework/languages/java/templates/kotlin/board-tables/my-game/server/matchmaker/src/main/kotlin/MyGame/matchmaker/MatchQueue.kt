package MyGame.matchmaker

import MyGame.MatchReason

internal class Ticket(val id: String, val playerId: String, val expiresAt: Long) {
    var disconnectDeadline: Long? = null
}

internal class Assignment(val id: String, val tickets: List<Ticket>, val deadline: Long) {
    val entered = mutableSetOf<String>()
}

internal class MatchQueue {
    private var waiting = linkedMapOf<String, Ticket>()
    private val assignments = mutableMapOf<String, Assignment>()

    fun enqueue(ticket: Ticket): Ticket =
        waiting[ticket.id]
            ?: assignmentFor(ticket.id)?.tickets?.first { it.id == ticket.id }
            ?: ticket.also { waiting[it.id] = it }

    fun reserve(deadline: Long, roomId: () -> String): Assignment? {
        val pair =
            MatchRule.select(waiting.values.asSequence().filter { it.disconnectDeadline == null })
        if (pair.size != 2) return null
        for (ticket in pair) waiting.remove(ticket.id)
        return Assignment(roomId(), pair, deadline).also { assignments[it.id] = it }
    }

    fun cancelWaiting(id: String): Ticket? = waiting.remove(id)

    fun disconnected(id: String, deadline: Long) {
        val ticket = waiting[id] ?: return
        if (ticket.disconnectDeadline == null) ticket.disconnectDeadline = deadline
    }

    fun expired(now: Long): List<Pair<Ticket, String>> =
        waiting.values
            .mapNotNull { ticket ->
                val disconnectDeadline = ticket.disconnectDeadline
                when {
                    disconnectDeadline != null &&
                        disconnectDeadline < ticket.expiresAt &&
                        now >= disconnectDeadline -> ticket to MatchReason.DISCONNECTED
                    now >= ticket.expiresAt -> ticket to MatchReason.EXPIRED
                    else -> null
                }
            }
            .also { expired -> for ((ticket, _) in expired) waiting.remove(ticket.id) }

    fun overdue(now: Long): List<Assignment> = assignments.values.filter { now >= it.deadline }

    fun assigned(id: String): Assignment? = assignments[id]

    fun assignmentFor(ticketId: String): Assignment? =
        assignments.values.firstOrNull { match -> match.tickets.any { it.id == ticketId } }

    fun finish(assignment: Assignment, requeue: Set<String> = emptySet()) {
        assignments.remove(assignment.id)
        if (requeue.isNotEmpty()) {
            waiting =
                LinkedHashMap<String, Ticket>().apply {
                    for (ticket in assignment.tickets) {
                        if (ticket.id in requeue) put(ticket.id, ticket)
                    }
                    putAll(waiting)
                }
        }
    }
}
