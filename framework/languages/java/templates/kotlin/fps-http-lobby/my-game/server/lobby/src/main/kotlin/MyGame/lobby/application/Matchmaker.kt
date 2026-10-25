package MyGame.lobby.application

import MyGame.MatchRequest
import MyGame.MatchResponse
import MyGame.Names
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage

fun interface RoomProvisioner {
    fun prepare(roomId: String, members: List<String>): CompletionStage<MatchResponse>
}

class Matchmaker(private val rooms: RoomProvisioner) : AutoCloseable {
    private data class WaitingPlayer(val playerId: String, val result: CompletableFuture<MatchResponse>)

    private val waiting = mutableMapOf<String, WaitingPlayer>()
    private val participants = mutableMapOf<String, CompletableFuture<MatchResponse>>()
    private var closed = false

    fun match(request: MatchRequest): CompletionStage<MatchResponse> {
        require(request.playerId.isNotBlank() && request.mode.isNotBlank()) {
            "Player and mode are required"
        }
        val result = CompletableFuture<MatchResponse>()
        var partner: WaitingPlayer? = null
        var roomId: String? = null
        var members: List<String> = emptyList()
        synchronized(this) {
            check(!closed) { "Lobby is closed" }
            require(request.playerId !in participants) { "Player already waiting" }
            partner = waiting.remove(request.mode)
            participants[request.playerId] = result
            if (partner == null) {
                waiting[request.mode] = WaitingPlayer(request.playerId, result)
            } else {
                roomId = Names.ROOM_PREFIX + UUID.randomUUID().toString().replace("-", "")
                participants[requireNotNull(partner).playerId] = requireNotNull(partner).result
                members = listOf(requireNotNull(partner).playerId, request.playerId)
            }
        }
        result.whenComplete { _, _ ->
            synchronized(this) {
                participants.remove(request.playerId, result)
                if (waiting[request.mode]?.result === result) waiting.remove(request.mode)
            }
        }
        val matched = partner
        if (matched != null) {
            try {
                rooms.prepare(requireNotNull(roomId), members).whenComplete { assignment, error ->
                    if (error == null) {
                        matched.result.complete(assignment)
                        result.complete(assignment)
                    } else {
                        matched.result.completeExceptionally(error)
                        result.completeExceptionally(error)
                    }
                }
            } catch (error: Exception) {
                matched.result.completeExceptionally(error)
                result.completeExceptionally(error)
            }
        }
        return result
    }

    fun cancelWaiting(request: MatchRequest, result: CompletableFuture<MatchResponse>): Boolean =
        synchronized(this) {
            val current = waiting[request.mode]
            if (current?.playerId != request.playerId || current.result !== result) return@synchronized false
            waiting.remove(request.mode)
            result.cancel(false)
        }

    override fun close() {
        val pending = synchronized(this) {
            closed = true
            participants.values.toList()
        }
        pending.forEach { it.completeExceptionally(IllegalStateException("Lobby shutdown")) }
    }
}
