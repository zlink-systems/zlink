package MyGame.match

import MyGame.*
import kotlin.test.*

class DomainTest {
    private class TestClock(var nanos: Long = 0L) {
        fun now() = nanos

        fun advanceMillis(milliseconds: Long) {
            nanos += milliseconds * NANOS_PER_MILLI
        }

        companion object {
            private const val NANOS_PER_MILLI = 1_000_000L
        }
    }

    private fun room(clock: TestClock = TestClock()) =
        RoomState(clock::now).also {
            assertTrue(it.admit("a"))
            assertTrue(it.admit("b"))
        }

    private fun positioned(
        clock: TestClock,
        ax: Double,
        ay: Double,
        bx: Double = 10.0,
        by: Double = 10.0,
    ) =
        room(clock).also {
            it.players[0].x = ax
            it.players[0].y = ay
            it.players[1].x = bx
            it.players[1].y = by
        }

    private fun assertRejectedWithoutChange(
        room: RoomState,
        reason: String,
        message: String? = null,
        action: () -> ActionRejected?,
    ) {
        val before = room.capture()
        assertEquals(reason, action()?.reason, message)
        assertContentEquals(before, room.capture())
    }

    @Test
    fun queueOwnsModesAndRoomSequence() {
        val state = MatchmakingState()
        state.enqueue(QueueRequest("p1"))
        state.enqueue(QueueRequest("p1"))
        state.enqueue(QueueRequest("p2", "ranked"))
        assertTrue(state.pair().isEmpty())
        state.enqueue(QueueRequest("p3"))
        assertEquals(listOf("p1", "p3"), state.pair().map { it.playerId })
        assertEquals("room-1", state.allocateRoom())
        state.remove(state.pair())
        assertEquals(listOf("ranked"), state.waiting.keys.toList())
        assertEquals("room-2", state.allocateRoom())
    }

    @Test
    fun acceptedActionsAdvanceOnlyValidSequences() {
        val state = room()
        assertEquals(Game.FIRST_SPAWN_X, state.view().first().x)
        assertEquals(Game.FIRST_SPAWN_Y, state.view().first().y)
        assertEquals(Game.SECOND_SPAWN_X, state.view().last().x)
        assertEquals(Game.SECOND_SPAWN_Y, state.view().last().y)
        assertNull(state.move("a", MoveRequest(0.0, 0.0, 1)))
        assertEquals(1L, state.view().first().lastSequence)
        assertRejectedWithoutChange(state, Rejection.STALE_SEQUENCE) {
            state.move("a", MoveRequest(0.0, 0.0, 1))
        }
        assertRejectedWithoutChange(state, Rejection.STALE_SEQUENCE) {
            state.shoot("a", ShootRequest("b", 1))
        }
        assertRejectedWithoutChange(state, Rejection.INTEGER_OUT_OF_RANGE) {
            state.move("a", MoveRequest(0.0, 0.0, Game.MAX_SAFE_INTEGER + 1))
        }

        val overlapping = room()
        overlapping.players.first().lastSequence = Game.MAX_SAFE_INTEGER + 1
        assertRejectedWithoutChange(overlapping, Rejection.INTEGER_OUT_OF_RANGE) {
            overlapping.move("a", MoveRequest(0.0, 0.0, Game.MAX_SAFE_INTEGER + 1))
        }

        assertNull(state.move("a", MoveRequest(0.0, 0.0, Game.MAX_SAFE_INTEGER)))
        assertEquals(Game.MAX_SAFE_INTEGER, state.view().first().lastSequence)
        assertRejectedWithoutChange(state, Rejection.INTEGER_OUT_OF_RANGE) {
            state.move("a", MoveRequest(0.0, 0.0, Game.MAX_SAFE_INTEGER + 1))
        }
    }

    @Test
    fun stepAndMapBoundariesAreInclusive() {
        val step = room()
        assertNull(step.move("a", MoveRequest(Game.MAX_STEP, 0.0, 1)))
        assertEquals(Game.FIRST_SPAWN_X + Game.MAX_STEP, step.view().first().x)
        assertRejectedWithoutChange(step, Rejection.STEP_TOO_LARGE) {
            step.move("a", MoveRequest(Game.MAX_STEP + 0.001, 0.0, 2))
        }

        val edges =
            listOf(
                Triple("x lower inclusive", 2.0 to 50.0, -2.0 to 0.0),
                Triple("x upper inclusive", 98.0 to 50.0, 2.0 to 0.0),
                Triple("y lower inclusive", 50.0 to 2.0, 0.0 to -2.0),
                Triple("y upper inclusive", 50.0 to 98.0, 0.0 to 2.0),
            )
        for ((name, start, delta) in edges) {
            val state = positioned(TestClock(), start.first, start.second)
            assertNull(state.move("a", MoveRequest(delta.first, delta.second, 1)), name)
            assertEquals(start.first + delta.first, state.view().first().x, name)
            assertEquals(start.second + delta.second, state.view().first().y, name)
        }

        val outside =
            listOf(
                Triple("x lower outside", 0.0 to 50.0, -0.01 to 0.0),
                Triple("x upper outside", 100.0 to 50.0, 0.01 to 0.0),
                Triple("y lower outside", 50.0 to 0.0, 0.0 to -0.01),
                Triple("y upper outside", 50.0 to 100.0, 0.0 to 0.01),
            )
        for ((name, start, delta) in outside) {
            val state = positioned(TestClock(), start.first, start.second)
            assertRejectedWithoutChange(state, Rejection.OUTSIDE_MAP, name) {
                state.move("a", MoveRequest(delta.first, delta.second, 1))
            }
        }
    }

    @Test
    fun closedWallEndpointsAndCrossingsBlockMovement() {
        val endpoints =
            listOf(
                "wall lower endpoint contact" to Game.WALL_MIN_Y,
                "wall upper endpoint contact" to Game.WALL_MAX_Y,
            )
        for ((name, y) in endpoints) {
            val state = positioned(TestClock(), Game.WALL_X - Game.MAX_STEP, y)
            assertRejectedWithoutChange(state, Rejection.MOVE_BLOCKED_BY_WALL, name) {
                state.move("a", MoveRequest(Game.MAX_STEP, 0.0, 1))
            }
        }

        val crossing = positioned(TestClock(), Game.WALL_X - 1.0, 40.0)
        assertRejectedWithoutChange(crossing, Rejection.MOVE_BLOCKED_BY_WALL) {
            crossing.move("a", MoveRequest(Game.MAX_STEP, 0.0, 1))
        }

        val clearLane = positioned(TestClock(), Game.WALL_X - Game.MAX_STEP, Game.WALL_MIN_Y - 1.0)
        assertNull(clearLane.move("a", MoveRequest(Game.MAX_STEP, 0.0, 1)))
    }

    @Test
    fun movementBudgetUsesFirstFullStepThenElapsedSpeedAndMaxStepCap() {
        val clock = TestClock()
        val state = room(clock)
        assertNull(state.move("a", MoveRequest(Game.MAX_STEP, 0.0, 1)))
        clock.advanceMillis(100)
        assertNull(state.move("a", MoveRequest(1.0, 0.0, 2)))

        val beforeRetry = state.capture()
        val rejection = state.move("a", MoveRequest(0.1, 0.0, 3))
        assertEquals(Rejection.MOVE_BUDGET, rejection?.reason)
        assertEquals(10, rejection?.retryAfterMs)
        assertContentEquals(beforeRetry, state.capture())
        clock.advanceMillis(10)
        assertNull(state.move("a", MoveRequest(0.1, 0.0, 3)))

        clock.advanceMillis(1_000)
        assertNull(state.move("a", MoveRequest(Game.MAX_STEP, 0.0, 4)))
        assertEquals(Game.MAX_STEP, state.view().first().x - (Game.FIRST_SPAWN_X + Game.MAX_STEP + 1.1))
    }

    @Test
    fun nonfiniteMovementIsRejectedWithoutChangingRoomState() {
        for (value in listOf(Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY)) {
            val horizontal = room()
            assertRejectedWithoutChange(horizontal, Rejection.INVALID_MOVE) {
                horizontal.move("a", MoveRequest(value, 0.0, 1))
            }
            val vertical = room()
            assertRejectedWithoutChange(vertical, Rejection.INVALID_MOVE) {
                vertical.move("a", MoveRequest(0.0, value, 1))
            }
        }
    }

    @Test
    fun shotsCheckWallRangeTargetsAndInclusiveRange() {
        val clear = positioned(TestClock(), 40.0, 10.0, 60.0, 10.0)
        assertNull(clear.shoot("a", ShootRequest("b", 1)))
        assertEquals(Game.INITIAL_HP - Game.SHOT_DAMAGE, clear.view().last().hp)

        val wall = positioned(TestClock(), 40.0, wallCenterY(), 60.0, wallCenterY())
        assertRejectedWithoutChange(wall, Rejection.BLOCKED_BY_WALL) {
            wall.shoot("a", ShootRequest("b", 1))
        }

        val far = positioned(TestClock(), 0.0, 10.0, 80.01, 10.0)
        assertRejectedWithoutChange(far, Rejection.OUT_OF_RANGE) {
            far.shoot("a", ShootRequest("b", 1))
        }

        val edge = positioned(TestClock(), 0.0, 10.0, Game.SHOT_RANGE, 10.0)
        assertNull(edge.shoot("a", ShootRequest("b", 1)))

        val missing = room()
        assertRejectedWithoutChange(missing, Rejection.NO_TARGET) {
            missing.shoot("a", ShootRequest("missing", 1))
        }
    }

    @Test
    fun fireIntervalReportsRemainingTimeAndAcceptsExactBoundary() {
        val clock = TestClock()
        val state = room(clock)
        assertNull(state.shoot("a", ShootRequest("b", 1)))
        assertRejectedWithoutChange(state, Rejection.FIRE_TOO_SOON) {
            state.shoot("a", ShootRequest("b", 2))
        }
        assertEquals(Game.FIRE_INTERVAL_MS.toInt(), state.shoot("a", ShootRequest("b", 2))?.retryAfterMs)

        clock.advanceMillis(Game.FIRE_INTERVAL_MS - 1)
        assertRejectedWithoutChange(state, Rejection.FIRE_TOO_SOON) {
            state.shoot("a", ShootRequest("b", 2))
        }
        assertEquals(1, state.shoot("a", ShootRequest("b", 2))?.retryAfterMs)

        clock.advanceMillis(1)
        assertNull(state.shoot("a", ShootRequest("b", 2)))
    }

    @Test
    fun snapshotStoresRemainingTimeAndRestoresUnderDifferentClockOrigin() {
        val sourceClock = TestClock(5_000_000_000L)
        val source = room(sourceClock)
        assertNull(source.move("a", MoveRequest(Game.MAX_STEP, 0.0, 1)))
        assertNull(source.shoot("a", ShootRequest("b", 2)))
        sourceClock.advanceMillis(60)
        val snapshot = source.capture()
        val sourcePlayer = Json.mapper.readTree(snapshot).get("players").get(0)
        assertEquals(140L, sourcePlayer.get("moveBudgetRemainingMs").asLong())
        assertEquals(190L, sourcePlayer.get("fireCooldownRemainingMs").asLong())
        val snapshotText = String(snapshot, Charsets.UTF_8)
        assertFalse(snapshotText.contains("Timestamp"))
        assertContentEquals(snapshot, source.capture())

        val destinationClock = TestClock(900_000_000_000_000L)
        val destination = RoomState(destinationClock::now)
        destination.restore(snapshot)
        assertEquals(source.view(), destination.view())
        assertEquals(source.phase, destination.phase)
        assertEquals(2L, destination.view().first().lastSequence)
        assertEquals(140, destination.move("a", MoveRequest(Game.MAX_STEP, 0.0, 3))?.retryAfterMs)
        assertEquals(190, destination.shoot("a", ShootRequest("b", 3))?.retryAfterMs)

        destinationClock.advanceMillis(140)
        assertNull(destination.move("a", MoveRequest(Game.MAX_STEP, 0.0, 3)))
        destinationClock.advanceMillis(50)
        assertNull(destination.shoot("a", ShootRequest("b", 4)))
    }

    @Test
    fun finishedMatchRejectsActionsWithoutChangingState() {
        val clock = TestClock()
        val state = room(clock)
        for (sequence in 1L..(Game.INITIAL_HP / Game.SHOT_DAMAGE)) {
            assertNull(state.shoot("a", ShootRequest("b", sequence)))
            clock.advanceMillis(Game.FIRE_INTERVAL_MS)
        }
        assertEquals(Game.FINISHED_PHASE, state.phase)
        assertEquals("a", state.winner)
        assertEquals(0, state.view().last().hp)
        assertRejectedWithoutChange(state, Rejection.MATCH_FINISHED) {
            state.move("a", MoveRequest(0.0, 0.0, 5))
        }
        assertRejectedWithoutChange(state, Rejection.MATCH_FINISHED) {
            state.shoot("a", ShootRequest("b", 5))
        }
    }

    private fun wallCenterY() = (Game.WALL_MIN_Y + Game.WALL_MAX_Y) / 2
}
