package MyGame.region.domain

import MyGame.*

data class Position(val x: Double = 0.0, val sequence: Long = 0, val remainingMs: Long = 0)

class Positions(private val clock: () -> Long = System::nanoTime) {
    private data class State(val x: Double, val sequence: Long, val budget: Double, val updatedAt: Long)
    private val players = linkedMapOf<String, State>()

    fun admit(id: String, capacity: Int): Boolean {
        if (id !in players && players.size >= capacity) return false
        players.putIfAbsent(id, State(Names.REGION_MIN_X.toDouble(), 0, Names.MAX_STEP.toDouble(), clock()))
        return true
    }
    fun leave(id: String) { players.remove(id) }
    fun move(id: String, delta: Double, sequence: Long): ActionRejected? {
        val state = requireNotNull(players[id]) { "Player is not in Region" }
        if (sequence > Names.MAX_SAFE_INTEGER) return ActionRejected("integer-out-of-range", 0)
        if (sequence <= state.sequence) return ActionRejected("stale-sequence", 0)
        if (!delta.isFinite()) return ActionRejected("non-finite", 0)
        val distance = kotlin.math.abs(delta)
        if (distance > Names.MAX_STEP) return ActionRejected("step-too-large", 0)
        val next = state.x + delta
        if (next < Names.REGION_MIN_X || next > Names.REGION_MAX_X) return ActionRejected("outside-region", 0)
        val now = clock()
        val available = available(state, now)
        if (distance > available) return ActionRejected("move-budget", maxOf(1, kotlin.math.ceil((distance - available) / Names.MAX_SPEED * 1000.0).toInt()))
        players[id] = State(next, sequence, available - distance, now)
        return null
    }
    fun view() = players.entries.sortedBy { it.key }.map { PlayerPosition(it.key, it.value.x, it.value.sequence) }
    fun snapshot(): Map<String, Position> {
        val now = clock()
        return players.entries.sortedBy { it.key }.associateTo(linkedMapOf()) {
            val remaining = maxOf(0L, kotlin.math.ceil((Names.MAX_STEP - available(it.value, now)) / Names.MAX_SPEED * 1000.0).toLong())
            it.key to Position(it.value.x, it.value.sequence, remaining)
        }
    }
    fun restore(snapshot: Map<String, Position>) {
        val now = clock()
        val restored = linkedMapOf<String, State>()
        for ((id, p) in snapshot) {
            require(id.isNotBlank() && p.x.isFinite() && p.x >= Names.REGION_MIN_X && p.x <= Names.REGION_MAX_X && p.sequence >= 0 && p.sequence <= Names.MAX_SAFE_INTEGER && p.remainingMs >= 0 && p.remainingMs <= kotlin.math.ceil(Names.MAX_STEP / Names.MAX_SPEED * 1000.0)) { "Invalid Region position snapshot" }
            restored[id] = State(p.x, p.sequence, Names.MAX_STEP - p.remainingMs * Names.MAX_SPEED / 1000.0, now)
        }
        players.clear()
        players.putAll(restored)
    }
    private fun available(state: State, now: Long): Double {
        val elapsed = (now - state.updatedAt) / 1_000_000_000.0
        return if (elapsed <= 0) state.budget else minOf(Names.MAX_STEP.toDouble(), state.budget + elapsed * Names.MAX_SPEED)
    }
}
