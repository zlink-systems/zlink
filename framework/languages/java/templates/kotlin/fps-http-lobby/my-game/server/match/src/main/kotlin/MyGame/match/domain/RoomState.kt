package MyGame.match.domain

import MyGame.*

class RoomState(private val nowMs: () -> Double = { System.nanoTime() / 1_000_000.0 }) {
    val players = mutableListOf<Combatant>()
    var phase: MatchPhase = MatchPhase.Waiting
        private set
    var winner: String? = null
        private set

    fun admit(playerId: String): Boolean {
        if (players.any { it.playerId == playerId }) return true
        if (players.size >= Game.PLAYERS_PER_ROOM) return false
        players.add(Combatant(playerId, ArenaMap.spawns[players.size]))
        if (players.size == Game.PLAYERS_PER_ROOM) phase = MatchPhase.Playing
        return true
    }

    private fun player(id: String) = players.single { it.playerId == id }

    private fun validateAction(player: Combatant, sequence: Long): String? =
        when {
            sequence <= player.sequence -> Rejection.STALE_SEQUENCE
            phase == MatchPhase.Finished -> Rejection.MATCH_FINISHED
            else -> null
        }

    fun move(id: String, request: MoveRequest): ActionRejected? {
        val player = player(id)
        validateAction(player, request.sequence)?.let { return ActionRejected(it) }
        if (!request.delta.isFinite) return ActionRejected(Rejection.NON_FINITE)
        if (request.delta.y != 0.0) return ActionRejected(Rejection.VERTICAL_MOVE)
        val distance = request.delta.length
        if (distance > Game.MAX_STEP) return ActionRejected(Rejection.STEP_TOO_LARGE)
        val next = player.position + request.delta
        if (!ArenaMap.bounds.contains(next)) return ActionRejected(Rejection.OUTSIDE_MAP)
        if (ArenaMap.walls.any { it.intersect(player.position, request.delta, 1.0) != null }) {
            return ActionRejected(Rejection.MOVE_BLOCKED_BY_WALL)
        }
        val now = nowMs()
        player.lastMoveMs?.let { lastMove ->
            val elapsed = maxOf(0.0, now - lastMove)
            val budget = minOf(Game.MAX_STEP, Game.MAX_SPEED * elapsed / 1000.0)
            if (distance > budget) {
                val retryAfterMs = maxOf(1, kotlin.math.ceil(distance * 1000.0 / Game.MAX_SPEED - elapsed).toInt())
                return ActionRejected(Rejection.MOVE_BUDGET, retryAfterMs)
            }
        }
        player.position = next
        player.sequence = request.sequence
        player.lastMoveMs = now
        return null
    }

    fun shoot(id: String, request: ShootRequest): ShotDecision {
        val shooter = player(id)
        validateAction(shooter, request.sequence)?.let {
            return ShotDecision(ShotRejected(it), null)
        }
        val direction = request.aimDirection.normalizedOrNull()
            ?: return ShotDecision(ShotRejected(Rejection.INVALID_AIM), null)
        val now = nowMs()
        shooter.nextFireMs?.let { nextFire ->
            if (now < nextFire) {
                val remaining = maxOf(1, kotlin.math.ceil(nextFire - now).toInt())
                return ShotDecision(ShotRejected(Rejection.FIRE_TOO_SOON, remaining), null)
            }
        }
        val origin = shooter.position + Vec3(0.0, Game.EYE_HEIGHT, 0.0)
        val wallDistance = ArenaMap.walls.mapNotNull {
            it.intersect(origin, direction, Game.SHOT_RANGE)
        }.minOrNull() ?: Double.POSITIVE_INFINITY

        var target: Combatant? = null
        var targetDistance = Double.POSITIVE_INFINITY
        for (candidate in players) {
            if (candidate == shooter || candidate.hp == 0) continue
            val distance = ArenaMap.playerBox(candidate.position)
                .intersect(origin, direction, Game.SHOT_RANGE)
            if (distance != null && distance < targetDistance) {
                target = candidate
                targetDistance = distance
            }
        }

        val outcome = when {
            wallDistance <= targetDistance && wallDistance.isFinite() -> ShotOutcome.BLOCKED_BY_WALL
            target != null -> ShotOutcome.HIT
            else -> ShotOutcome.MISS
        }
        if (outcome == ShotOutcome.HIT) {
            val hit = requireNotNull(target)
            hit.hp = maxOf(0, hit.hp - Game.SHOT_DAMAGE)
            if (hit.hp == 0) {
                phase = MatchPhase.Finished
                winner = id
            }
        }
        shooter.sequence = request.sequence
        shooter.nextFireMs = now + Game.FIRE_INTERVAL_MS
        return ShotDecision(
            null,
            ShotResult(outcome, if (outcome == ShotOutcome.HIT) target?.playerId else null, request.sequence),
        )
    }

    fun view() = players.map { PlayerView(it.playerId, it.position, it.hp, it.sequence) }

    fun capture(): RoomSnapshot {
        val now = nowMs()
        return RoomSnapshot(
            players.map { player ->
                CombatantSnapshot(
                    player.playerId,
                    player.position,
                    player.hp,
                    player.sequence,
                    player.nextFireMs?.let { maxOf(0.0, it - now) } ?: 0.0,
                    player.lastMoveMs?.let {
                        maxOf(0.0, Game.MAX_STEP * 1000.0 / Game.MAX_SPEED - (now - it))
                    } ?: 0.0,
                )
            },
            phase,
            winner,
        )
    }

    fun restore(snapshot: RoomSnapshot) {
        val now = nowMs()
        players.clear()
        snapshot.players.forEach { player ->
            players.add(
                Combatant(player.playerId, player.position).also {
                    it.hp = player.hp
                    it.sequence = player.sequence
                    it.nextFireMs = now + player.fireRemainingMs
                    it.lastMoveMs = if (player.moveRemainingMs > 0.0) {
                        now - (Game.MAX_STEP * 1000.0 / Game.MAX_SPEED - player.moveRemainingMs)
                    } else null
                },
            )
        }
        phase = snapshot.phase
        winner = snapshot.winner
    }
}

class Combatant(val playerId: String, position: Vec3) {
    var position: Vec3 = position
        internal set
    var hp: Int = Game.INITIAL_HP
        internal set
    var sequence: Long = 0
        internal set
    internal var lastMoveMs: Double? = null
    internal var nextFireMs: Double? = null
}

data class ShotDecision(val rejected: ShotRejected?, val result: ShotResult?)

data class CombatantSnapshot(
    val playerId: String,
    val position: Vec3,
    val hp: Int,
    val sequence: Long,
    val fireRemainingMs: Double,
    val moveRemainingMs: Double,
)

data class RoomSnapshot(
    val players: List<CombatantSnapshot>,
    val phase: MatchPhase,
    val winner: String?,
)
