package MyGame

import com.fasterxml.jackson.databind.annotation.JsonSerialize

@JsonSerialize class FindMatch

data class MatchQueued(val ticketId: String)

data class CancelMatch(val ticketId: String)

data class MatchCancelled(val ticketId: String, val reason: String)

data class MatchFound(val ticketId: String, val roomId: String, val seat: Int)

data class MatchRequest(val requestKey: String, val playerId: String)

data class PrepareRoom(val assignmentId: String, val members: List<String>)

data class RoomPrepared(val assignmentId: String, val ok: Boolean)

data class MatchAssigned(val ticketId: String, val assignmentId: String, val roomId: String)

data class EnterReport(val ticketId: String, val assignmentId: String, val ok: Boolean)

data class CommitRoom(val assignmentId: String)

data class WithdrawRoom(val assignmentId: String)

data class CancelTicket(val ticketId: String)

data class TicketDisconnected(val ticketId: String)

object MatchReason {
    const val CANCELLED = "cancelled"
    const val EXPIRED = "expired"
    const val DISCONNECTED = "disconnected"
}
