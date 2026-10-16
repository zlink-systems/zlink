package MyGame.region.domain

import MyGame.PlayerPosition

data class Position(val x: Int = 0, val sequence: Long = 0)

class Positions {
    private var players = linkedMapOf<String, Position>()

    fun admit(id: String, capacity: Int): Boolean {
        if (id !in players && players.size >= capacity) return false
        players.putIfAbsent(id, Position())
        return true
    }

    fun leave(id: String) {
        players.remove(id)
    }

    fun move(id: String, x: Int, sequence: Long) {
        val current = requireNotNull(players[id]) { "Player is not in Region" }
        require(sequence > current.sequence) { "Move sequence must increase" }
        players[id] = Position(x, sequence)
    }

    fun view() = players.entries.sortedBy { it.key }.map { PlayerPosition(it.key, it.value.x) }

    fun snapshot(): Map<String, Position> = LinkedHashMap(players)

    fun restore(state: Map<String, Position>) {
        players = LinkedHashMap(state)
    }
}
