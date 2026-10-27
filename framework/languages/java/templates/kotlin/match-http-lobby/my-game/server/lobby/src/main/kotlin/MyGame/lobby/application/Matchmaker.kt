package MyGame.lobby.application

import MyGame.*
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage

fun interface RoomProvisioner {
    fun prepare(roomId: String, members: List<String>): CompletionStage<MatchResponse>
}

class Matchmaker(private val rooms: RoomProvisioner) : AutoCloseable {
    private data class Pending(
        val playerId: String,
        val mode: String,
        val result: CompletableFuture<MatchResponse>,
    )

    private val waiting = mutableMapOf<String, Pending>()
    private val participants = mutableMapOf<String, Pending>()
    private var closed = false

    fun match(request: MatchRequest): CompletionStage<MatchResponse> {
        require(request.playerId.isNotBlank()) { "Player is required" }
        val mode = request.mode ?: Names.DEFAULT_MODE
        require(mode.isNotBlank()) { "Mode is required" }

        val result = CompletableFuture<MatchResponse>()
        val current = Pending(request.playerId, mode, result)
        val partner: Pending?
        val roomId: String?
        synchronized(this) {
            check(!closed) { "Lobby is closed" }
            require(current.playerId !in participants) { "Player already waiting" }
            participants[current.playerId] = current
            partner = waiting.remove(mode)
            if (partner == null) {
                waiting[mode] = current
                roomId = null
            } else {
                roomId = Names.ROOM_PREFIX + UUID.randomUUID().toString().replace("-", "")
            }
        }
        result.whenComplete { _, _ ->
            synchronized(this) {
                participants.remove(current.playerId, current)
                waiting.remove(current.mode, current)
            }
        }

        if (partner != null) {
            val matchId = requireNotNull(roomId)
            val members = listOf(partner.playerId, current.playerId)
            try {
                rooms.prepare(matchId, members).whenComplete { assignment, error ->
                    if (error != null) {
                        partner.result.completeExceptionally(error)
                        result.completeExceptionally(error)
                    } else if (assignment == null || assignment.roomId != matchId) {
                        val failure = IllegalStateException("Prepared Room identity mismatch")
                        partner.result.completeExceptionally(failure)
                        result.completeExceptionally(failure)
                    } else {
                        partner.result.complete(assignment)
                        result.complete(assignment)
                    }
                }
            } catch (error: Exception) {
                partner.result.completeExceptionally(error)
                result.completeExceptionally(error)
            }
        }
        return result
    }

    @Synchronized
    fun cancelWaiting(request: MatchRequest, result: CompletableFuture<MatchResponse>): Boolean {
        val pending = participants[request.playerId] ?: return false
        if (pending.result !== result || !waiting.remove(pending.mode, pending)) return false
        result.cancel(false)
        return true
    }

    override fun close() {
        val pending = synchronized(this) {
            closed = true
            participants.values.map { it.result }
        }
        pending.forEach { it.completeExceptionally(IllegalStateException("Lobby shutdown")) }
    }
}
