package MyGame.field

import MyGame.*

class FieldPositionDomain(
    private val fieldId: String,
    monotonicMilliseconds: () -> Long = { System.nanoTime() / 1_000_000L },
) {
    private val clock = monotonicMilliseconds
    private val players = linkedMapOf<String, PlayerState>()
    private val arrivals = linkedMapOf<String, EnterField>()

    companion object {
        fun resolveLocation(x: Double): FieldLocationResolution {
            if (!x.isFinite()) return FieldLocationResolution(null, ActionRejected(Rejection.NON_FINITE))
            if (x < 0 || x >= Rules.FIELD_WIDTH * Rules.FIELD_COUNT)
                return FieldLocationResolution(null, ActionRejected(Rejection.OUTSIDE_WORLD))
            return FieldLocationResolution(Rules.fieldId(x), null)
        }
    }

    fun queueArrival(actorId: String, arrival: EnterField): ActionRejected? {
        val location = resolveLocation(arrival.x)
        location.rejection?.let { return it }
        if (arrival.sequence > Rules.MAX_SAFE_INTEGER)
            return ActionRejected(Rejection.INTEGER_OUT_OF_RANGE)
        if (arrival.sequence < 0) return ActionRejected(Rejection.STALE_SEQUENCE)
        if (arrival.remainingMs < 0 || arrival.remainingMs > Rules.FULL_BUDGET_MS)
            return ActionRejected(Rejection.INTEGER_OUT_OF_RANGE)
        if (location.fieldId != fieldId) return ActionRejected(Rejection.OUTSIDE_WORLD)
        arrivals[actorId] = arrival
        return null
    }

    fun takeArrival(actorId: String): EnterField? = arrivals.remove(actorId)

    fun addPlayer(actorId: String, arrival: EnterField, connected: Boolean = true) {
        val acceptedAt = clock() - (Rules.FULL_BUDGET_MS - arrival.remainingMs)
        players[actorId] = PlayerState(
            Position(arrival.x),
            arrival.sequence,
            acceptedAt,
            connected,
        )
    }

    fun move(actorId: String, request: MoveRequest): MovementResult {
        val player = players[actorId]
            ?: return MovementResult(
                false,
                Position(0.0),
                0L,
                0,
                ActionRejected(Rejection.STALE_SEQUENCE),
                fieldId,
                false,
            )
        if (!request.delta.isFinite()) return reject(actorId, Rejection.NON_FINITE)
        if (kotlin.math.abs(request.delta) > Rules.MAX_STEP)
            return reject(actorId, Rejection.STEP_TOO_LARGE)
        if (request.sequence > Rules.MAX_SAFE_INTEGER)
            return reject(actorId, Rejection.INTEGER_OUT_OF_RANGE)
        if (request.sequence < 0 || player.sequence >= Rules.MAX_SAFE_INTEGER || request.sequence <= player.sequence)
            return reject(actorId, Rejection.STALE_SEQUENCE)

        val nextX = player.position.x + request.delta
        val location = resolveLocation(nextX)
        location.rejection?.let { return reject(actorId, it.reason) }

        val now = clock()
        val elapsedMs = (now - player.lastAcceptedMilliseconds).coerceAtLeast(0)
        val budget = minOf(Rules.MAX_STEP, Rules.MAX_SPEED * elapsedMs / 1000.0)
        val distance = kotlin.math.abs(request.delta)
        if (distance > budget) {
            val retryAfterMs = maxOf(1, kotlin.math.ceil((distance - budget) * 1000 / Rules.MAX_SPEED).toInt())
            return reject(actorId, Rejection.MOVE_BUDGET, retryAfterMs)
        }

        val position = Position(nextX)
        val nextFieldId = requireNotNull(location.fieldId)
        val relocates = nextFieldId != fieldId
        if (!relocates)
            players[actorId] = player.copy(
                position = position,
                sequence = request.sequence,
                lastAcceptedMilliseconds = now,
            )
        return MovementResult(
            true,
            position,
            request.sequence,
            Rules.FULL_BUDGET_MS,
            null,
            nextFieldId,
            relocates,
        )
    }

    fun state(actorId: String): PlayerStateView {
        val player = players.getValue(actorId)
        return PlayerStateView(
            player.position,
            player.sequence,
            remainingMs(player.lastAcceptedMilliseconds, clock()),
            player.connected,
        )
    }

    fun removePlayer(actorId: String): PlayerStateView? {
        val player = players.remove(actorId) ?: return null
        return PlayerStateView(
            player.position,
            player.sequence,
            remainingMs(player.lastAcceptedMilliseconds, clock()),
            player.connected,
        )
    }

    fun setConnected(actorId: String, connected: Boolean): PlayerStateView? {
        val player = players[actorId] ?: return null
        players[actorId] = player.copy(connected = connected)
        return state(actorId)
    }

    fun capture(): FieldPositionSnapshot {
        val now = clock()
        val snapshots = players.mapValuesTo(linkedMapOf()) { (_, player) ->
            FieldPlayerSnapshot(
                player.position,
                player.sequence,
                remainingMs(player.lastAcceptedMilliseconds, now),
                player.connected,
            )
        }
        return FieldPositionSnapshot(fieldId, snapshots, LinkedHashMap(arrivals))
    }

    fun restore(snapshot: FieldPositionSnapshot) {
        require(snapshot.fieldId == fieldId) { "Field relocation state belongs to a different Field." }
        players.clear()
        val now = clock()
        for ((actorId, state) in snapshot.players) {
            val acceptedAt = now - (Rules.FULL_BUDGET_MS - state.remainingMs)
            players[actorId] = PlayerState(state.position, state.sequence, acceptedAt, state.connected)
        }
        arrivals.clear()
        arrivals.putAll(snapshot.arrivals)
    }

    private fun reject(actorId: String, reason: String, retryAfterMs: Int = 0): MovementResult {
        val player = players.getValue(actorId)
        return MovementResult(
            false,
            player.position,
            player.sequence,
            remainingMs(player.lastAcceptedMilliseconds, clock()),
            ActionRejected(reason, retryAfterMs),
            fieldId,
            false,
        )
    }

    private fun remainingMs(acceptedAt: Long, now: Long): Int =
        (Rules.FULL_BUDGET_MS - (now - acceptedAt).coerceAtLeast(0))
            .coerceIn(0L, Rules.FULL_BUDGET_MS.toLong())
            .toInt()

    private data class PlayerState(
        val position: Position,
        val sequence: Long,
        val lastAcceptedMilliseconds: Long,
        val connected: Boolean,
    )
}

data class Position(val x: Double)

data class MovementResult(
    val accepted: Boolean,
    val position: Position,
    val sequence: Long,
    val remainingMs: Int,
    val rejection: ActionRejected?,
    val fieldId: String,
    val relocates: Boolean,
)

data class FieldLocationResolution(val fieldId: String?, val rejection: ActionRejected?)

data class PlayerStateView(
    val position: Position,
    val sequence: Long,
    val remainingMs: Int,
    val connected: Boolean,
)

data class FieldPlayerSnapshot(
    val position: Position,
    val sequence: Long,
    val remainingMs: Int,
    val connected: Boolean,
)

data class FieldPositionSnapshot(
    val fieldId: String,
    val players: Map<String, FieldPlayerSnapshot>,
    val arrivals: Map<String, EnterField>,
)
