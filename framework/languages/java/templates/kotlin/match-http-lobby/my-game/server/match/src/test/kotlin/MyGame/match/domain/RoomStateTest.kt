package MyGame.match.domain

import MyGame.*
import org.junit.jupiter.api.Assertions.*
import org.junit.jupiter.api.Test

class RoomStateTest {
    private fun room() =
        RoomState().also {
            assertTrue(it.admit("a"))
            assertTrue(it.admit("b"))
        }

    @Test
    fun hitAndWinner() {
        val room = room()
        assertNull(room.move("a", MoveRequest(10.0, 10.0, 1)))
        assertNull(room.move("b", MoveRequest(20.0, 10.0, 1)))
        for (i in 1..Rules.INITIAL_HP / Rules.DAMAGE) {
            assertNull(room.shoot("a", ShootRequest("b", i + 1L)))
            assertEquals(Rules.INITIAL_HP - Rules.DAMAGE * i, room.view()[1].hp)
        }
        assertEquals("a", room.winner)
        assertEquals(Rules.FINISHED, room.move("b", MoveRequest(0.0, 0.0, 2)))
    }

    @Test
    fun closedWallAndRangeBoundary() {
        val room = room()
        room.move("a", MoveRequest(10.0, 0.0, 1))
        room.move("b", MoveRequest(60.0, 0.0, 1))
        assertEquals(Rules.BLOCKED, room.shoot("a", ShootRequest("b", 2)))
        room.move("a", MoveRequest(50.0, -10.0, 3))
        room.move("b", MoveRequest(50.0, 110.0, 2))
        assertEquals(Rules.OUT_OF_RANGE, room.shoot("a", ShootRequest("b", 4)))
        room.move("a", MoveRequest(50.0, -10.0, 5))
        room.move("b", MoveRequest(50.0, 0.0, 3))
        assertEquals(Rules.BLOCKED, room.shoot("a", ShootRequest("b", 6)))
        room.move("a", MoveRequest(0.0, -1.0, 7))
        room.move("b", MoveRequest(Rules.RANGE, -1.0, 4))
        assertNull(room.shoot("a", ShootRequest("b", 8)))
        room.move("b", MoveRequest(Rules.RANGE + 1, -1.0, 5))
        assertEquals(Rules.OUT_OF_RANGE, room.shoot("a", ShootRequest("b", 9)))
    }

    @Test
    fun rejectedActionsRetainGameStateAndConsumeSequence() {
        val room = room()
        val before = room.view()
        assertEquals(Rules.INVALID_MOVE, room.move("a", MoveRequest(Double.NaN, 0.0, 1)))
        assertEquals(before, room.view())
        assertEquals(Rules.STALE_SEQUENCE, room.move("a", MoveRequest(1.0, 1.0, 1)))
        assertEquals(Rules.NO_TARGET, room.shoot("a", ShootRequest("a", 2)))
        assertEquals(Rules.NO_TARGET, room.shoot("a", ShootRequest("missing", 3)))
        assertEquals(before, room.view())
        assertTrue(room.admit("a"))
        assertFalse(room.admit("third"))
    }

    @Test
    fun relocationPreservesApplicationState() {
        val room = room()
        room.move("b", MoveRequest(20.0, -1.0, 1))
        room.shoot("a", ShootRequest("b", 1))
        val restored =
            Json.mapper.readValue(Json.mapper.writeValueAsBytes(room), RoomState::class.java)
        assertEquals(room.view(), restored.view())
        assertEquals(room.winner, restored.winner)
        assertEquals(Rules.STALE_SEQUENCE, restored.shoot("a", ShootRequest("b", 1)))
    }
}
