package MyGame.zone

import MyGame.*
import kotlin.math.abs
import kotlin.math.ceil

data class PlayerStateView(
    val position: Position,
    val sequence: Long,
    val remainingMs: Int,
    val connected: Boolean,
)

data class ZonePlayerSnapshot(
    val position: Position,
    val sequence: Long,
    val remainingMs: Int,
    val connected: Boolean,
)

data class ZonePositionSnapshot(
    val zoneId: String,
    val players: Map<String, ZonePlayerSnapshot>,
    val arrivals: Map<String, EnterZone>,
)

data class ZoneLocationResolution(val zoneId: String? = null, val rejection: ActionRejected? = null)

data class MovementResult(
    val accepted: Boolean,
    val position: Position,
    val sequence: Long,
    val remainingMs: Int,
    val rejection: ActionRejected? = null,
    val zoneId: String,
    val relocates: Boolean,
)

class ZonePositionDomain(
    private val zoneId: String,
    private val clock: () -> Long = { System.nanoTime() / 1_000_000L },
) {
    private data class PlayerState(
        val position: Position,
        val sequence: Long,
        val lastAcceptedMs: Long,
        val connected: Boolean,
    )

    private val players = mutableMapOf<String, PlayerState>()
    private val arrivals = mutableMapOf<String, EnterZone>()

    companion object {
        fun resolveLocation(x: Double): ZoneLocationResolution {
            if (!x.isFinite()) return ZoneLocationResolution(rejection = ActionRejected("non-finite"))
            if (x < 0 || x >= Rules.WORLD_WIDTH)
                return ZoneLocationResolution(rejection = ActionRejected("outside-world"))
            return ZoneLocationResolution(Rules.zoneId(x))
        }
    }

    fun queueArrival(actorId: String, arrival: EnterZone): ActionRejected? {
        val location = resolveLocation(arrival.position.x)
        location.rejection?.let { return it }
        if (arrival.sequence > Rules.MAX_SAFE_INTEGER)
            return ActionRejected("integer-out-of-range")
        if (arrival.sequence < 0) return ActionRejected("stale-sequence")
        if (arrival.remainingMs !in 0..Rules.FULL_BUDGET_AFTER_MS)
            return ActionRejected("integer-out-of-range")
        if (location.zoneId != zoneId) return ActionRejected("outside-world")
        check(arrivals.putIfAbsent(actorId, arrival) == null) { "Duplicate Zone arrival" }
        return null
    }

    fun takeArrival(actorId: String): EnterZone? = arrivals.remove(actorId)

    fun addPlayer(actorId: String, arrival: EnterZone, connected: Boolean = true) {
        val acceptedAt = clock() - (Rules.FULL_BUDGET_AFTER_MS - arrival.remainingMs)
        players[actorId] =
            PlayerState(arrival.position, arrival.sequence, acceptedAt, connected)
    }

    fun move(actorId: String, request: MoveRequest): MovementResult {
        val player = players[actorId]
            ?: return MovementResult(
                false,
                Position(0.0),
                0,
                0,
                ActionRejected("stale-sequence"),
                zoneId,
                false,
            )
        if (!request.delta.isFinite()) return reject(player, "non-finite")
        if (abs(request.delta) > Rules.MAX_STEP) return reject(player, "step-too-large")
        if (request.sequence > Rules.MAX_SAFE_INTEGER)
            return reject(player, "integer-out-of-range")
        if (
            request.sequence < 0 ||
                player.sequence >= Rules.MAX_SAFE_INTEGER ||
                request.sequence <= player.sequence
        )
            return reject(player, "stale-sequence")

        val position = Position(player.position.x + request.delta)
        val location = resolveLocation(position.x)
        location.rejection?.let { return reject(player, it.reason) }

        val now = clock()
        val elapsedMs = (now - player.lastAcceptedMs).coerceAtLeast(0)
        val budget = minOf(Rules.MAX_STEP, Rules.MAX_SPEED * elapsedMs / 1000.0)
        val distance = abs(request.delta)
        if (distance > budget) {
            val retryAfterMs = maxOf(1.0, ceil((distance - budget) * 1000 / Rules.MAX_SPEED)).toInt()
            return reject(player, "move-budget", retryAfterMs)
        }

        val nextZone = requireNotNull(location.zoneId)
        val relocates = nextZone != zoneId
        if (!relocates)
            players[actorId] = player.copy(position = position, sequence = request.sequence, lastAcceptedMs = now)
        return MovementResult(
            true,
            position,
            request.sequence,
            Rules.FULL_BUDGET_AFTER_MS,
            zoneId = nextZone,
            relocates = relocates,
        )
    }

    fun state(actorId: String): PlayerStateView = players[actorId]?.view(clock())
        ?: error("Player is not present in Zone: $actorId")

    fun tryState(actorId: String): PlayerStateView? = players[actorId]?.view(clock())

    fun connectedPlayers(): Map<String, PlayerStateView> {
        val now = clock()
        return players.filterValues { it.connected }.mapValues { (_, player) -> player.view(now) }
    }

    fun setConnected(actorId: String, connected: Boolean): PlayerStateView? {
        val player = players[actorId] ?: return null
        players[actorId] = player.copy(connected = connected)
        return state(actorId)
    }

    fun removePlayer(actorId: String): PlayerStateView? = players.remove(actorId)?.view(clock())

    fun capture(): ZonePositionSnapshot {
        val now = clock()
        val playerSnapshots = players.mapValues { (_, player) ->
            ZonePlayerSnapshot(player.position, player.sequence, remainingMs(player.lastAcceptedMs, now), player.connected)
        }
        return ZonePositionSnapshot(zoneId, playerSnapshots, arrivals.toMap())
    }

    fun restore(snapshot: ZonePositionSnapshot) {
        require(snapshot.zoneId == zoneId) { "Zone relocation state belongs to a different Zone" }
        players.clear()
        arrivals.clear()
        val now = clock()
        snapshot.players.forEach { (actorId, state) ->
            players[actorId] = PlayerState(
                state.position,
                state.sequence,
                now - (Rules.FULL_BUDGET_AFTER_MS - state.remainingMs),
                state.connected,
            )
        }
        arrivals.putAll(snapshot.arrivals)
    }

    private fun reject(player: PlayerState, reason: String, retryAfterMs: Int = 0) =
        MovementResult(
            false,
            player.position,
            player.sequence,
            remainingMs(player.lastAcceptedMs, clock()),
            ActionRejected(reason, retryAfterMs),
            zoneId,
            false,
        )

    private fun PlayerState.view(now: Long) =
        PlayerStateView(position, sequence, remainingMs(lastAcceptedMs, now), connected)

    private fun remainingMs(acceptedAt: Long, now: Long): Int =
        (Rules.FULL_BUDGET_AFTER_MS - (now - acceptedAt).coerceAtLeast(0))
            .coerceIn(0, Rules.FULL_BUDGET_AFTER_MS.toLong())
            .toInt()
}
