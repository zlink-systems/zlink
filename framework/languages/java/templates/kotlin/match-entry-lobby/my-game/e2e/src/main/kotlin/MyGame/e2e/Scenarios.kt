package MyGame.e2e

import MyGame.*
import java.util.UUID
import kotlinx.coroutines.delay

private val WALL_CENTER_Y = (Game.WALL_MIN_Y + Game.WALL_MAX_Y) / 2
private const val FAR_X = Game.MAP_MAX_X
private const val FAR_Y = Game.MAP_MAX_Y - 10
private const val LANE_Y = Game.FIRST_SPAWN_Y

private fun sameState(before: RoomView, after: RoomView) {
    check(before.copy(nodeId = after.nodeId) == after) { "Room state changed: $before -> $after" }
}

private suspend fun match(a: Client, b: Client): RoomView {
    a.queue()
    b.queue()
    val first = a.next<MatchFound>()
    val second = b.next<MatchFound>()
    check(first.roomId == second.roomId) { "Players entered different rooms." }
    val full: (RoomView) -> Boolean = { it.players.size == Game.PLAYERS_PER_ROOM }
    val room = a.next(full)
    check(room == b.next<RoomView>(full)) { "Initial room views differ." }
    check(
        room.players.all { it.hp == Game.INITIAL_HP && it.lastSequence == 0L } &&
            room.phase == Game.PLAYING_PHASE &&
            room.winner == null
    ) {
        "Initial room state differs."
    }
    return room
}

private suspend fun nextSequence(player: Client): Long {
    val view = player.view()
    return view.players.single { it.playerId == player.playerId }.lastSequence + 1
}

private suspend fun rejectAction(player: Client, request: Any, reason: String): ActionRejected {
    val before = player.view()
    player.clearActionResults()
    player.send(request)
    val rejected = player.next<ActionRejected>()
    check(rejected.reason == reason) { "Expected $reason; received ${rejected.reason}." }
    val timeLimited = reason == Rejection.MOVE_BUDGET || reason == Rejection.FIRE_TOO_SOON
    check(if (timeLimited) rejected.retryAfterMs > 0 else rejected.retryAfterMs == 0) {
        "Unexpected RetryAfterMs for $reason."
    }
    sameState(before, player.view())
    return rejected
}

private suspend fun rejectShot(
    a: Client,
    b: Client,
    sequence: Long,
    reason: String,
    target: String = b.playerId,
) = rejectAction(a, ShootRequest(target, sequence), reason)

private suspend fun walk(
    a: Client,
    b: Client,
    mover: Client,
    destinationX: Double,
    destinationY: Double,
): RoomView {
    var view = mover.view()
    var player = view.players.single { it.playerId == mover.playerId }
    while (player.x != destinationX || player.y != destinationY) {
        val remainingX = destinationX - player.x
        val dx = if (remainingX == 0.0) 0.0 else remainingX.coerceIn(-Game.MAX_STEP, Game.MAX_STEP)
        val remainingY = destinationY - player.y
        val dy =
            if (dx != 0.0 || remainingY == 0.0) 0.0
            else remainingY.coerceIn(-Game.MAX_STEP, Game.MAX_STEP)
        view = moveBy(a, b, mover, dx, dy, player.lastSequence + 1, view)
        player = view.players.single { it.playerId == mover.playerId }
    }
    return view
}

private suspend fun moveBy(
    a: Client,
    b: Client,
    mover: Client,
    dx: Double,
    dy: Double,
    sequence: Long,
    priorView: RoomView? = null,
    requireMoveBudget: Boolean = false,
): RoomView {
    val before = priorView ?: mover.view()
    val current = before.players.single { it.playerId == mover.playerId }
    val targetX = current.x + dx
    val targetY = current.y + dy
    val moved: (RoomView) -> Boolean = { view ->
        view.players.any {
            it.playerId == mover.playerId &&
                it.x == targetX &&
                it.y == targetY &&
                it.lastSequence == sequence
        }
    }
    val request = MoveRequest(dx, dy, sequence)
    var moveBudgetRejected = false
    while (true) {
        mover.clearActionResults()
        mover.send(request)
        when (val result = mover.nextActionResult(moved)) {
            is ActionRejected -> {
                check(!moveBudgetRejected) { "Move was rejected again after RetryAfterMs." }
                moveBudgetRejected = true
                check(
                    result.reason == Rejection.MOVE_BUDGET && result.retryAfterMs > 0
                ) {
                    "Unexpected movement rejection: ${result.reason}."
                }
                sameState(before, mover.view())
                delay(result.retryAfterMs.toLong())
            }
            is RoomView -> {
                val other = if (mover.playerId == a.playerId) b else a
                val peer = other.next(moved)
                check(result == peer) { "Movement views differ." }
                check(!requireMoveBudget || moveBudgetRejected) {
                    "Move budget rejection was not observed."
                }
                return result
            }
            else -> error("Unexpected action result: $result")
        }
    }
}

private suspend fun hit(
    a: Client,
    b: Client,
    sequence: Long,
    hp: Int,
    allowFireRetry: Boolean = true,
): RoomView {
    fun isHit(view: RoomView) =
        view.players.any { it.playerId == b.playerId && it.hp == hp } &&
            (hp != 0 || (view.phase == Game.FINISHED_PHASE && view.winner == a.playerId))

    var before = a.view()
    val request = ShootRequest(b.playerId, sequence)
    var fireTooSoonRejected = false
    while (true) {
        a.clearActionResults()
        a.send(request)
        when (val result = a.nextActionResult(::isHit)) {
            is ActionRejected -> {
                check(!fireTooSoonRejected) { "Shot was rejected again after RetryAfterMs." }
                fireTooSoonRejected = true
                check(
                    result.reason == Rejection.FIRE_TOO_SOON && result.retryAfterMs > 0
                ) {
                    "Unexpected shot rejection: ${result.reason}."
                }
                sameState(before, a.view())
                check(allowFireRetry) { "Shot was rejected after the explicit RetryAfterMs wait." }
                delay(result.retryAfterMs.toLong())
                before = a.view()
            }
            is RoomView -> {
                val peer = b.next<RoomView>(::isHit)
                check(result == peer) { "Hit views differ." }
                return result
            }
            else -> error("Unexpected action result: $result")
        }
    }
}

private suspend fun newMatch(endpoint: String, replacement: String) {
    val suffix = UUID.randomUUID().toString()
    val a = Client(endpoint, "new-a-$suffix")
    val b = Client(endpoint, "new-b-$suffix")
    withCleanup(cleanup = { withCleanup(cleanup = { a.close() }) { b.close() } }) {
        a.connect()
        b.connect()
        check(match(a, b).nodeId == replacement) { "Replacement did not accept the new room." }
    }
}

private suspend fun input(a: Client, b: Client) {
    hit(a, b, 1, Game.INITIAL_HP - Game.SHOT_DAMAGE)
    val fireDelay = rejectAction(a, ShootRequest(b.playerId, 2), Rejection.FIRE_TOO_SOON)
    delay(fireDelay.retryAfterMs.toLong())
    hit(a, b, 2, Game.INITIAL_HP - Game.SHOT_DAMAGE * 2, allowFireRetry = false)

    val initial = a.view()
    val firstSpawn = initial.players.single { it.x == Game.FIRST_SPAWN_X && it.y == Game.FIRST_SPAWN_Y }
    val mover = if (firstSpawn.playerId == a.playerId) a else b
    val movementStart =
        initial.players.single { it.playerId == mover.playerId }.lastSequence + 1
    val budgetSequence = movementStart + 1
    val firstMove = moveBy(a, b, mover, 1.0, 0.0, movementStart)
    moveBy(a, b, mover, 1.0, 0.0, budgetSequence, firstMove, requireMoveBudget = true)
    rejectAction(mover, MoveRequest(0.0, 0.0, budgetSequence), Rejection.STALE_SEQUENCE)
    rejectAction(mover, MoveRequest(0.0, 0.0, movementStart), Rejection.STALE_SEQUENCE)
    val stepTooLargeSequence = budgetSequence + 1
    rejectAction(
        mover,
        MoveRequest(Game.MAX_STEP + 0.01, 0.0, stepTooLargeSequence),
        Rejection.STEP_TOO_LARGE,
    )
    rejectAction(
        mover,
        MoveRequest(0.0, 0.0, Game.MAX_SAFE_INTEGER + 1),
        Rejection.INTEGER_OUT_OF_RANGE,
    )

    val outsideStart = walk(a, b, mover, 1.0, LANE_Y)
    val outsideSequence =
        outsideStart.players.single { it.playerId == mover.playerId }.lastSequence + 1
    rejectAction(
        mover,
        MoveRequest(-Game.MAX_STEP, 0.0, outsideSequence),
        Rejection.OUTSIDE_MAP,
    )

    val beforeWall = walk(a, b, mover, Game.WALL_X - Game.MAX_STEP, WALL_CENTER_Y)
    val wallSequence = beforeWall.players.single { it.playerId == mover.playerId }.lastSequence + 1
    rejectAction(
        mover,
        MoveRequest(Game.MAX_STEP, 0.0, wallSequence),
        Rejection.MOVE_BLOCKED_BY_WALL,
    )

    moveBy(a, b, mover, -0.1, 0.0, Game.MAX_SAFE_INTEGER)
    val atMaximum = mover.view()
    check(
        atMaximum.players.single { it.playerId == mover.playerId }.lastSequence ==
            Game.MAX_SAFE_INTEGER
    ) {
        "Maximum safe sequence was not accepted."
    }
    rejectAction(
        mover,
        MoveRequest(0.0, 0.0, Game.MAX_SAFE_INTEGER + 1),
        Rejection.INTEGER_OUT_OF_RANGE,
    )
}

suspend fun scenario(id: String, nodes: Nodes) {
    val suffix = UUID.randomUUID().toString()
    val a = Client(nodes.endpoint, "p1-$suffix")
    val b = Client(nodes.endpoint, "p2-$suffix")
    var bClosed = false
    withCleanup(cleanup = { withCleanup(cleanup = { a.close() }) { if (!bClosed) b.close() } }) {
        a.connect()
        b.connect()
        val initial = match(a, b)
        if (id == "input") {
            input(a, b)
            check(a.disconnects == 0 && a.connector.isConnected) { "First player disconnected." }
            check(b.disconnects == 0 && b.connector.isConnected) { "Second player disconnected." }
            return@withCleanup
        }

        val session1 = a.sessionInfo()
        val session2 = b.sessionInfo()
        moveBy(a, b, a, 0.0, 0.0, 1, initial)
        moveBy(a, b, b, 0.0, 0.0, 1, initial)
        val before = hit(a, b, 2, Game.INITIAL_HP - Game.SHOT_DAMAGE)
        when (id) {
            "hit" -> {
                rejectAction(a, RoomAssignment(before.roomId), Rejection.PACKET_NOT_ALLOWED)
                walk(a, b, a, Game.FIRST_SPAWN_X, Game.FIRST_SPAWN_Y)
                walk(a, b, b, Game.SECOND_SPAWN_X, WALL_CENTER_Y)
                rejectShot(a, b, nextSequence(a), Rejection.BLOCKED_BY_WALL)
                walk(a, b, b, FAR_X, FAR_Y)
                rejectShot(a, b, nextSequence(a), Rejection.OUT_OF_RANGE)
                rejectShot(a, b, nextSequence(a), Rejection.NO_TARGET, "missing-player")
                walk(a, b, b, Game.SECOND_SPAWN_X, LANE_Y)
                for (hp in Game.INITIAL_HP - 2 * Game.SHOT_DAMAGE downTo 0 step Game.SHOT_DAMAGE)
                    hit(a, b, nextSequence(a), hp)
                check(
                    a.next<MatchEnded>().winner == a.playerId &&
                        b.next<MatchEnded>().winner == a.playerId
                ) {
                    "Wrong match winner."
                }
            }
            "wall" -> {
                val left = Game.WALL_X - Game.MAX_STEP
                val right = Game.WALL_X + Game.MAX_STEP
                walk(a, b, a, left, WALL_CENTER_Y)
                walk(a, b, b, right, WALL_CENTER_Y)
                val beforeWall = a.view()
                val targetBefore = beforeWall.players.single { it.playerId == b.playerId }
                rejectAction(
                    a,
                    MoveRequest(Game.MAX_STEP, 0.0, nextSequence(a)),
                    Rejection.MOVE_BLOCKED_BY_WALL,
                )
                rejectShot(a, b, nextSequence(a), Rejection.BLOCKED_BY_WALL)
                val afterWall = a.view()
                sameState(beforeWall, afterWall)
                check(
                    afterWall.players.single { it.playerId == b.playerId }.hp == targetBefore.hp
                ) {
                    "Wall-blocked shot changed HP."
                }

                walk(a, b, a, Game.FIRST_SPAWN_X, Game.FIRST_SPAWN_Y)
                walk(a, b, b, Game.SECOND_SPAWN_X, Game.SECOND_SPAWN_Y)
                val beforeClearShot = a.view()
                val expectedHp =
                    beforeClearShot.players.single { it.playerId == b.playerId }.hp - Game.SHOT_DAMAGE
                val clearShot = hit(a, b, nextSequence(a), expectedHp)
                check(clearShot.players.single { it.playerId == b.playerId }.hp == expectedHp) {
                    "Wall behind target blocked a clear shot."
                }
            }
            "reconnect" -> {
                bClosed = true
                b.close()
                val again = Client(nodes.endpoint, b.playerId)
                withCleanup(cleanup = { again.close() }) {
                    again.connect()
                    again.queue()
                    val restored = again.next<RoomView>()
                    val identity = again.sessionInfo()
                    check(
                        identity.sessionId != session2.sessionId &&
                            identity.actorId == session2.actorId &&
                            identity.objectGeneration == session2.objectGeneration
                    ) {
                        "Reconnect changed Actor identity."
                    }
                    sameState(before, restored)
                    check(
                        restored.phase == Game.PLAYING_PHASE &&
                            restored.winner == null &&
                            restored.players.single { it.playerId == again.playerId }.lastSequence == 1L
                    ) {
                        "Reconnect did not restore the active phase and last sequence."
                    }
                    moveBy(a, again, again, 0.0, 0.0, 2)
                    hit(a, again, 3, Game.INITIAL_HP - Game.SHOT_DAMAGE * 2)
                    hit(a, again, 4, Game.INITIAL_HP - Game.SHOT_DAMAGE * 3)
                    val finished = hit(a, again, 5, 0)
                    check(finished.phase == Game.FINISHED_PHASE && finished.winner == a.playerId) {
                        "Room view did not restore the finished phase and winner."
                    }

                    val finishedReconnect = Client(nodes.endpoint, again.playerId)
                    withCleanup(cleanup = { finishedReconnect.close() }) {
                        finishedReconnect.connect()
                        finishedReconnect.queue()
                        val restoredFinished =
                            finishedReconnect.next<RoomView> { it.phase == Game.FINISHED_PHASE }
                        val finishedIdentity = finishedReconnect.sessionInfo()
                        check(
                            finishedIdentity.sessionId != identity.sessionId &&
                                finishedIdentity.actorId == identity.actorId &&
                                finishedIdentity.objectGeneration == identity.objectGeneration
                        ) {
                            "Finished reconnect changed Actor identity."
                        }
                        sameState(finished, restoredFinished)
                        check(
                            restoredFinished.winner == a.playerId &&
                                restoredFinished.players.single {
                                    it.playerId == again.playerId
                                }.lastSequence == 2L
                        ) {
                            "Finished reconnect did not restore the winner and last sequence."
                        }
                    }
                }
            }
            "update" -> {
                nodes.drain(before.nodeId)
                check(a.sessionInfo() == session1 && b.sessionInfo() == session2) {
                    "Update changed Session binding."
                }
                sameState(before, a.view { it.nodeId != before.nodeId })
                sameState(before, b.view { it.nodeId != before.nodeId })
                nodes.replace(before.nodeId)
                newMatch(nodes.endpoint, before.nodeId)
                hit(a, b, 3, Game.INITIAL_HP - Game.SHOT_DAMAGE * 2)
                check(a.sessionInfo() == session1 && b.sessionInfo() == session2) {
                    "Replacement changed Session binding."
                }
            }
            else -> error("Unknown scenario: $id")
        }
        check(a.disconnects == 0 && a.connector.isConnected) { "First player disconnected." }
        if (id != "reconnect")
            check(b.disconnects == 0 && b.connector.isConnected) { "Second player disconnected." }
    }
}
