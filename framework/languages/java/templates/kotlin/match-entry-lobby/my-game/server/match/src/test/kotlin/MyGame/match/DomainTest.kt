package MyGame.match

import MyGame.*
import kotlin.test.*

class DomainTest {
    private fun room() =
        RoomState().also {
            assertTrue(it.admit("p1"))
            assertTrue(it.admit("p2"))
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
    fun hitRejectsWallRangeAndMissingTargetWithoutDamage() {
        val state = room()
        assertNull(state.move("p1", MoveRequest(10.0, 10.0, 1)))
        assertNull(state.move("p2", MoveRequest(20.0, 10.0, 1)))
        assertNull(state.shoot("p1", ShootRequest("p2", 2)))
        assertEquals(75, state.players.last().hp)
        assertNull(state.move("p2", MoveRequest(60.0, 10.0, 2)))
        assertEquals(Rejection.BLOCKED_BY_WALL, state.shoot("p1", ShootRequest("p2", 3)))
        assertNull(state.move("p2", MoveRequest(200.0, 10.0, 3)))
        assertEquals(Rejection.OUT_OF_RANGE, state.shoot("p1", ShootRequest("p2", 4)))
        assertEquals(Rejection.NO_TARGET, state.shoot("p1", ShootRequest("missing", 5)))
        assertEquals(75, state.players.last().hp)
    }

    @Test
    fun closedWallEndpointsAndInclusiveRange() {
        val state = room()
        state.move("p1", MoveRequest(10.0, 100.0, 1))
        state.move("p2", MoveRequest(90.0, 100.0, 1))
        assertEquals(Rejection.BLOCKED_BY_WALL, state.shoot("p1", ShootRequest("p2", 2)))
        state.move("p1", MoveRequest(10.0, 101.0, 3))
        state.move("p2", MoveRequest(90.0, 101.0, 2))
        assertNull(state.shoot("p1", ShootRequest("p2", 4)))
        state.move("p1", MoveRequest(50.0, 0.0, 5))
        state.move("p2", MoveRequest(50.0, 100.0, 3))
        assertEquals(Rejection.OUT_OF_RANGE, state.shoot("p1", ShootRequest("p2", 6)))
        state.move("p2", MoveRequest(50.0, 80.0, 4))
        assertEquals(Rejection.BLOCKED_BY_WALL, state.shoot("p1", ShootRequest("p2", 7)))
    }

    @Test
    fun terminalAndSequenceChecksPreserveCombatState() {
        val state = room()
        assertFalse(state.admit("p3"))
        assertTrue(state.admit("p1"))
        assertEquals(Rejection.INVALID_MOVE, state.move("p1", MoveRequest(Double.NaN, 0.0, 1)))
        assertEquals(Rejection.STALE_SEQUENCE, state.shoot("p1", ShootRequest("p2", 1)))
        for (sequence in 2L..5L) assertNull(state.shoot("p1", ShootRequest("p2", sequence)))
        assertEquals("p1", state.winner)
        assertEquals(0, state.players.last().hp)
        assertEquals(Rejection.MATCH_FINISHED, state.move("p2", MoveRequest(10.0, 0.0, 1)))
        assertEquals(Rejection.MATCH_FINISHED, state.shoot("p1", ShootRequest("p2", 6)))
    }
}
