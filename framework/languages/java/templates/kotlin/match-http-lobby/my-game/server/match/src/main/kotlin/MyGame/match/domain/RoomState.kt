package MyGame.match.domain

import MyGame.*

class RoomState {
    var players: MutableList<Combatant> = mutableListOf()
    var winner: String? = null

    data class Combatant(
        var playerId: String = "",
        var x: Double = 0.0,
        var y: Double = 0.0,
        var hp: Int = Rules.INITIAL_HP,
        var sequence: Long = 0,
    )

    fun admit(id: String): Boolean {
        if (players.any { it.playerId == id }) return true
        if (players.size >= Rules.PLAYERS) return false
        players.add(Combatant(id))
        return true
    }

    private fun player(id: String) = players.single { it.playerId == id }

    private fun validate(player: Combatant, sequence: Long): String? {
        if (sequence <= player.sequence) return Rules.STALE_SEQUENCE
        player.sequence = sequence
        return if (winner == null) null else Rules.FINISHED
    }

    fun move(id: String, request: MoveRequest): String? {
        val p = player(id)
        validate(p, request.sequence)?.let {
            return it
        }
        if (!request.x.isFinite() || !request.y.isFinite()) return Rules.INVALID_MOVE
        p.x = request.x
        p.y = request.y
        return null
    }

    fun shoot(id: String, request: ShootRequest): String? {
        val p = player(id)
        validate(p, request.sequence)?.let {
            return it
        }
        val target =
            players.find { it.playerId == request.targetId && it.playerId != id && it.hp > 0 }
                ?: return Rules.NO_TARGET
        val dx = target.x - p.x
        val dy = target.y - p.y
        if (dx * dx + dy * dy > Rules.RANGE * Rules.RANGE) return Rules.OUT_OF_RANGE
        val blocked =
            if (dx == 0.0) {
                p.x == Rules.WALL_X &&
                    maxOf(p.y, target.y) >= Rules.WALL_MIN_Y &&
                    minOf(p.y, target.y) <= Rules.WALL_MAX_Y
            } else {
                val t = (Rules.WALL_X - p.x) / dx
                val y = p.y + t * dy
                t >= 0 && t <= 1 && y >= Rules.WALL_MIN_Y && y <= Rules.WALL_MAX_Y
            }
        if (blocked) return Rules.BLOCKED
        target.hp = maxOf(0, target.hp - Rules.DAMAGE)
        if (target.hp == 0) winner = id
        return null
    }

    fun view() = players.map { PlayerView(it.playerId, it.x, it.y, it.hp) }
}
