package MyGame.match

import MyGame.*

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
        waiting.values.firstOrNull { it.size >= Game.PLAYERS_PER_ROOM }?.take(Game.PLAYERS_PER_ROOM)
            ?: emptyList()

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

data class Combatant(
    val playerId: String = "",
    var x: Double = 0.0,
    var y: Double = 0.0,
    var hp: Int = Game.INITIAL_HP,
    var sequence: Long = 0,
)

data class RoomState(
    val players: MutableList<Combatant> = mutableListOf(),
    var winner: String? = null,
) {
    fun admit(playerId: String): Boolean {
        if (players.any { it.playerId == playerId }) return true
        if (players.size >= Game.PLAYERS_PER_ROOM) return false
        players.add(Combatant(playerId))
        return true
    }

    private fun player(id: String) = players.single { it.playerId == id }

    private fun validate(player: Combatant, sequence: Long): String? {
        if (sequence <= player.sequence) return Rejection.STALE_SEQUENCE
        player.sequence = sequence
        return if (winner == null) null else Rejection.MATCH_FINISHED
    }

    fun move(id: String, request: MoveRequest): String? {
        val player = player(id)
        validate(player, request.sequence)?.let {
            return it
        }
        if (!request.x.isFinite() || !request.y.isFinite()) return Rejection.INVALID_MOVE
        player.x = request.x
        player.y = request.y
        return null
    }

    fun shoot(id: String, request: ShootRequest): String? {
        val shooter = player(id)
        validate(shooter, request.sequence)?.let {
            return it
        }
        val target =
            players.find { it.playerId == request.targetId && it.playerId != id && it.hp > 0 }
                ?: return Rejection.NO_TARGET
        val dx = target.x - shooter.x
        val dy = target.y - shooter.y
        if (dx * dx + dy * dy > Game.SHOT_RANGE * Game.SHOT_RANGE) return Rejection.OUT_OF_RANGE
        if (
            Game.WALLS.any { wall ->
                // Touching the closed wall segment blocks the shot.
                if (dx == 0.0)
                    shooter.x == wall.x &&
                        maxOf(shooter.y, target.y) >= wall.minY &&
                        minOf(shooter.y, target.y) <= wall.maxY
                else {
                    val t = (wall.x - shooter.x) / dx
                    val y = shooter.y + t * dy
                    t in 0.0..1.0 && y in wall.minY..wall.maxY
                }
            }
        )
            return Rejection.BLOCKED_BY_WALL
        target.hp = maxOf(0, target.hp - Game.SHOT_DAMAGE)
        if (target.hp == 0) winner = id
        return null
    }

    fun view() = players.map { PlayerView(it.playerId, it.x, it.y, it.hp) }
}
