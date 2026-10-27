package MyGame.match

import MyGame.Game
import MyGame.Names
import MyGame.QueueRequest

data class MatchmakingState(
    val waiting: MutableMap<String, MutableList<QueueRequest>> = linkedMapOf(),
    var roomSequence: Long = 0,
) {
    fun enqueue(request: QueueRequest) {
        require(request.mode.isNotBlank()) { "Mode required" }
        if (waiting.values.any { queue -> queue.any { it.playerId == request.playerId } }) return
        waiting.getOrPut(request.mode) { mutableListOf() }.add(request)
    }

    fun pair(): List<QueueRequest> =
        waiting.values.firstOrNull { it.size >= Game.PLAYERS_PER_ROOM }
            ?.take(Game.PLAYERS_PER_ROOM) ?: emptyList()

    fun allocateRoom(): String {
        roomSequence = Math.incrementExact(roomSequence)
        return Names.ROOM_PREFIX + roomSequence
    }

    fun remove(pair: List<QueueRequest>) {
        val mode = pair.first().mode
        val queue = requireNotNull(waiting[mode])
        queue.subList(0, Game.PLAYERS_PER_ROOM).clear()
        if (queue.isEmpty()) waiting.remove(mode)
    }
}
