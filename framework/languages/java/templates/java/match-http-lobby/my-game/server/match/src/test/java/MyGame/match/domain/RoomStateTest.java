package MyGame.match.domain;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import org.junit.jupiter.api.Test;

public class RoomStateTest {
    private RoomState room() {
        var room = new RoomState();
        assertTrue(room.admit("a"));
        assertTrue(room.admit("b"));
        return room;
    }

    @Test
    void hitAndWinner() {
        var room = room();
        assertNull(room.move("a", new MoveRequest(10, 10, 1)));
        assertNull(room.move("b", new MoveRequest(20, 10, 1)));
        for (int i = 1; i <= Rules.INITIAL_HP / Rules.DAMAGE; i++) {
            assertNull(room.shoot("a", new ShootRequest("b", i + 1)));
            assertEquals(Rules.INITIAL_HP - Rules.DAMAGE * i, room.view().get(1).hp());
        }
        assertEquals("a", room.winner);
        assertEquals(Rules.FINISHED, room.move("b", new MoveRequest(0, 0, 2)));
    }

    @Test
    void closedWallAndRangeBoundary() {
        var room = room();
        room.move("a", new MoveRequest(10, 0, 1));
        room.move("b", new MoveRequest(60, 0, 1));
        assertEquals(Rules.BLOCKED, room.shoot("a", new ShootRequest("b", 2)));
        room.move("a", new MoveRequest(50, -10, 3));
        room.move("b", new MoveRequest(50, 110, 2));
        assertEquals(Rules.OUT_OF_RANGE, room.shoot("a", new ShootRequest("b", 4)));
        room.move("a", new MoveRequest(50, -10, 5));
        room.move("b", new MoveRequest(50, 0, 3));
        assertEquals(Rules.BLOCKED, room.shoot("a", new ShootRequest("b", 6)));
        room.move("a", new MoveRequest(0, -1, 7));
        room.move("b", new MoveRequest(Rules.RANGE, -1, 4));
        assertNull(room.shoot("a", new ShootRequest("b", 8)));
        room.move("b", new MoveRequest(Rules.RANGE + 1, -1, 5));
        assertEquals(Rules.OUT_OF_RANGE, room.shoot("a", new ShootRequest("b", 9)));
    }

    @Test
    void rejectedActionsRetainGameStateAndConsumeSequence() {
        var room = room();
        var before = room.view();
        assertEquals(Rules.INVALID_MOVE, room.move("a", new MoveRequest(Double.NaN, 0, 1)));
        assertEquals(before, room.view());
        assertEquals(Rules.STALE_SEQUENCE, room.move("a", new MoveRequest(1, 1, 1)));
        assertEquals(Rules.NO_TARGET, room.shoot("a", new ShootRequest("a", 2)));
        assertEquals(Rules.NO_TARGET, room.shoot("a", new ShootRequest("missing", 3)));
        assertEquals(before, room.view());
        assertTrue(room.admit("a"));
        assertFalse(room.admit("third"));
    }

    @Test
    void relocationPreservesApplicationState() throws Exception {
        var room = room();
        room.move("b", new MoveRequest(20, -1, 1));
        room.shoot("a", new ShootRequest("b", 1));
        var restored = Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(room), RoomState.class);
        assertEquals(room.view(), restored.view());
        assertEquals(room.winner, restored.winner);
        assertEquals(Rules.STALE_SEQUENCE, restored.shoot("a", new ShootRequest("b", 1)));
    }
}
