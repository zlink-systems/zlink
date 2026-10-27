package MyGame.match

import MyGame.*
import kotlin.math.ceil
import kotlin.math.min
import kotlin.math.sqrt

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

internal data class Combatant(
    val playerId: String,
    var x: Double,
    var y: Double,
    var hp: Int = Game.INITIAL_HP,
    var lastSequence: Long = 0,
    var lastMoveTimestamp: Long? = null,
    var lastShotTimestamp: Long? = null,
)

internal data class CombatantSnapshot(
    val playerId: String,
    val x: Double,
    val y: Double,
    val hp: Int,
    val lastSequence: Long,
    val moveBudgetRemainingMs: Long?,
    val fireCooldownRemainingMs: Long?,
)

internal data class RoomStateSnapshot(
    val phase: String,
    val winner: String?,
    val players: List<CombatantSnapshot>,
)

class RoomState(private val clock: () -> Long = { System.nanoTime() }) {
    internal val players = mutableListOf<Combatant>()
    var winner: String? = null
        private set
    val phase: String
        get() = if (winner == null) Game.PLAYING_PHASE else Game.FINISHED_PHASE

    fun admit(playerId: String): Boolean {
        if (players.any { it.playerId == playerId }) return true
        if (players.size >= Game.PLAYERS_PER_ROOM) return false
        val spawn =
            if (players.isEmpty()) Game.FIRST_SPAWN_X to Game.FIRST_SPAWN_Y
            else Game.SECOND_SPAWN_X to Game.SECOND_SPAWN_Y
        players.add(Combatant(playerId, spawn.first, spawn.second))
        return true
    }

    private fun player(id: String) = players.single { it.playerId == id }

    private fun validateAction(player: Combatant, sequence: Long): ActionRejected? {
        if (sequence > Game.MAX_SAFE_INTEGER)
            return ActionRejected(Rejection.INTEGER_OUT_OF_RANGE)
        if (sequence <= player.lastSequence) return ActionRejected(Rejection.STALE_SEQUENCE)
        return if (winner == null) null else ActionRejected(Rejection.MATCH_FINISHED)
    }

    private fun commitAcceptedAction(player: Combatant, sequence: Long) {
        player.lastSequence = sequence
    }

    fun move(id: String, request: MoveRequest): ActionRejected? {
        val player = player(id)
        validateAction(player, request.sequence)?.let {
            return it
        }

        if (!request.dx.isFinite() || !request.dy.isFinite())
            return ActionRejected(Rejection.INVALID_MOVE)
        val distance = sqrt(request.dx * request.dx + request.dy * request.dy)
        if (distance > Game.MAX_STEP) return ActionRejected(Rejection.STEP_TOO_LARGE)

        val targetX = player.x + request.dx
        val targetY = player.y + request.dy
        if (
            targetX < Game.MAP_MIN_X ||
                targetX > Game.MAP_MAX_X ||
                targetY < Game.MAP_MIN_Y ||
                targetY > Game.MAP_MAX_Y
        )
            return ActionRejected(Rejection.OUTSIDE_MAP)
        if (Game.WALLS.any { intersectsWall(player.x, player.y, targetX, targetY, it) })
            return ActionRejected(Rejection.MOVE_BLOCKED_BY_WALL)

        val now = clock()
        if (player.lastMoveTimestamp != null) {
            val elapsedMs = elapsedMilliseconds(player.lastMoveTimestamp!!, now)
            val budget = min(Game.MAX_STEP, Game.MAX_SPEED * elapsedMs / 1000.0)
            if (distance > budget) {
                val retryAfterMs = retryAfterMilliseconds(
                    (distance - budget) * 1000.0 / Game.MAX_SPEED,
                )
                return ActionRejected(Rejection.MOVE_BUDGET, retryAfterMs)
            }
        }

        player.x = targetX
        player.y = targetY
        commitAcceptedAction(player, request.sequence)
        player.lastMoveTimestamp = now
        return null
    }

    fun shoot(id: String, request: ShootRequest): ActionRejected? {
        val shooter = player(id)
        validateAction(shooter, request.sequence)?.let {
            return it
        }
        val target =
            players.find { it.playerId == request.targetId && it.playerId != id && it.hp > 0 }
                ?: return ActionRejected(Rejection.NO_TARGET)
        val dx = target.x - shooter.x
        val dy = target.y - shooter.y
        if (dx * dx + dy * dy > Game.SHOT_RANGE * Game.SHOT_RANGE)
            return ActionRejected(Rejection.OUT_OF_RANGE)
        if (Game.WALLS.any { intersectsWall(shooter.x, shooter.y, target.x, target.y, it) })
            return ActionRejected(Rejection.BLOCKED_BY_WALL)

        val now = clock()
        if (shooter.lastShotTimestamp != null) {
            val retryAfterMs = Game.FIRE_INTERVAL_MS - elapsedMilliseconds(shooter.lastShotTimestamp!!, now)
            if (retryAfterMs > 0)
                return ActionRejected(
                    Rejection.FIRE_TOO_SOON,
                    retryAfterMilliseconds(retryAfterMs),
                )
        }

        target.hp = maxOf(0, target.hp - Game.SHOT_DAMAGE)
        if (target.hp == 0) winner = id
        commitAcceptedAction(shooter, request.sequence)
        shooter.lastShotTimestamp = now
        return null
    }

    fun view() = players.map { PlayerView(it.playerId, it.x, it.y, it.hp, it.lastSequence) }

    fun capture(): ByteArray {
        val now = clock()
        val snapshot =
            RoomStateSnapshot(
                phase,
                winner,
                players.map { player ->
                    CombatantSnapshot(
                        player.playerId,
                        player.x,
                        player.y,
                        player.hp,
                        player.lastSequence,
                        remainingMilliseconds(player.lastMoveTimestamp, MOVE_BUDGET_DURATION_MS, now),
                        remainingMilliseconds(player.lastShotTimestamp, Game.FIRE_INTERVAL_MS.toDouble(), now),
                    )
                },
            )
        return Json.mapper.writeValueAsBytes(snapshot)
    }

    fun restore(payload: ByteArray) {
        val snapshot = Json.mapper.readValue(payload, RoomStateSnapshot::class.java)
        val expectedPhase = if (snapshot.winner == null) Game.PLAYING_PHASE else Game.FINISHED_PHASE
        require(snapshot.phase == expectedPhase && snapshot.players.size <= Game.PLAYERS_PER_ROOM) {
            "Invalid room state snapshot"
        }

        val now = clock()
        players.clear()
        snapshot.players.forEach { player ->
            players.add(
                Combatant(
                    player.playerId,
                    player.x,
                    player.y,
                    player.hp,
                    player.lastSequence,
                    restoreTimestamp(player.moveBudgetRemainingMs, MOVE_BUDGET_DURATION_MS, now),
                    restoreTimestamp(
                        player.fireCooldownRemainingMs,
                        Game.FIRE_INTERVAL_MS.toDouble(),
                        now,
                    ),
                ),
            )
        }
        winner = snapshot.winner
    }

    private fun elapsedMilliseconds(startedAt: Long, now: Long): Double =
        ((now - startedAt).coerceAtLeast(0L)).toDouble() / NANOS_PER_MILLI

    private fun retryAfterMilliseconds(milliseconds: Double): Int {
        if (milliseconds.isNaN() || milliseconds <= 1.0) return 1
        if (milliseconds >= Int.MAX_VALUE.toDouble()) return Int.MAX_VALUE
        return ceil(milliseconds).toInt()
    }

    private fun remainingMilliseconds(startedAt: Long?, durationMs: Double, now: Long): Long? {
        if (startedAt == null) return null
        return ceil(durationMs - elapsedMilliseconds(startedAt, now)).toLong().coerceAtLeast(0L)
    }

    private fun restoreTimestamp(remainingMs: Long?, durationMs: Double, now: Long): Long? {
        if (remainingMs == null) return null
        val boundedRemainingMs = remainingMs.coerceIn(0L, durationMs.toLong())
        val elapsedMs = durationMs - boundedRemainingMs
        return now - ceil(elapsedMs * NANOS_PER_MILLI).toLong()
    }

    private fun intersectsWall(ax: Double, ay: Double, bx: Double, by: Double, wall: MyGame.Wall): Boolean {
        if (ax == bx)
            return ax == wall.x && maxOf(ay, by) >= wall.minY && minOf(ay, by) <= wall.maxY
        val t = (wall.x - ax) / (bx - ax)
        val y = ay + t * (by - ay)
        return t in 0.0..1.0 && y in wall.minY..wall.maxY
    }
    companion object {
        private const val NANOS_PER_MILLI = 1_000_000L
        private val MOVE_BUDGET_DURATION_MS = Game.MAX_STEP / Game.MAX_SPEED * 1000.0
    }
}
