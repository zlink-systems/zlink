package MyGame.lobby.application

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage

fun interface RoomProvisioner {
    fun prepare(roomId: String): CompletionStage<MatchResponse>
}

class Matchmaker(private val rooms: RoomProvisioner) : AutoCloseable {
    private val waiting = mutableMapOf<String, CompletableFuture<MatchResponse>>()
    private val participants = mutableMapOf<String, CompletableFuture<MatchResponse>>()
    private var sequence = 0L
    private var closed = false

    fun match(request: MatchRequest): CompletionStage<MatchResponse> {
        require(request.playerId.isNotBlank() && request.mode.isNotBlank()) {
            "Player and mode are required"
        }
        val result = CompletableFuture<MatchResponse>()
        val partner: CompletableFuture<MatchResponse>?
        var roomId: String? = null
        synchronized(this) {
            check(!closed) { "Lobby is closed" }
            require(request.playerId !in participants) { "Player already waiting" }
            if (request.mode in waiting) {
                sequence = Math.incrementExact(sequence)
                roomId = Names.ROOM_PREFIX + sequence
            }
            participants[request.playerId] = result
            partner = waiting.remove(request.mode)
            if (partner == null) waiting[request.mode] = result
        }
        result.whenComplete { _, _ ->
            synchronized(this) {
                participants.remove(request.playerId, result)
                waiting.remove(request.mode, result)
            }
        }
        if (partner != null) {
            try {
                rooms.prepare(requireNotNull(roomId)).whenComplete { value, error ->
                    if (error == null) {
                        partner.complete(value)
                        result.complete(value)
                    } else {
                        partner.completeExceptionally(error)
                        result.completeExceptionally(error)
                    }
                }
            } catch (error: Exception) {
                partner.completeExceptionally(error)
                result.completeExceptionally(error)
            }
        }
        return result
    }

    override fun close() {
        val pending =
            synchronized(this) {
                closed = true
                participants.values.toList()
            }
        pending.forEach { it.completeExceptionally(IllegalStateException("Lobby shutdown")) }
    }
}
