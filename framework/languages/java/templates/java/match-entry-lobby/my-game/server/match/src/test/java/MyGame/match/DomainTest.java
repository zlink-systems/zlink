package MyGame.match;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import org.junit.jupiter.api.Test;

final class DomainTest {
    private static final String SHOOTER = "p1", TARGET = "p2", OTHER_MODE = "squad";
    private static final double NEAR_X = 10,
            TARGET_X = 20,
            WALL_BEHIND_X = 60,
            FAR_X = 200,
            LANE_Y = 10;

    private static RoomState room() {
        var state = new RoomState();
        assertTrue(state.admit(SHOOTER));
        assertTrue(state.admit(TARGET));
        assertTrue(state.admit(SHOOTER));
        assertFalse(state.admit("p3"));
        assertNull(state.move(SHOOTER, new MoveRequest(NEAR_X, LANE_Y, 1)));
        assertNull(state.move(TARGET, new MoveRequest(TARGET_X, LANE_Y, 1)));
        return state;
    }

    @Test
    void combatRejectsWithoutChangingHealthAndEndsAtZero() {
        var state = room();
        assertNull(state.shoot(SHOOTER, new ShootRequest(TARGET, 2)));
        assertEquals(Rules.INITIAL_HP - Rules.SHOT_DAMAGE, state.players.getLast().hp);
        assertNull(state.move(TARGET, new MoveRequest(WALL_BEHIND_X, LANE_Y, 2)));
        assertEquals(Rules.BLOCKED_BY_WALL, state.shoot(SHOOTER, new ShootRequest(TARGET, 3)));
        assertNull(state.move(TARGET, new MoveRequest(FAR_X, LANE_Y, 3)));
        assertEquals(Rules.OUT_OF_RANGE, state.shoot(SHOOTER, new ShootRequest(TARGET, 4)));
        assertEquals(Rules.NO_TARGET, state.shoot(SHOOTER, new ShootRequest(SHOOTER, 5)));
        assertEquals(Rules.INITIAL_HP - Rules.SHOT_DAMAGE, state.players.getLast().hp);
        assertNull(state.move(TARGET, new MoveRequest(TARGET_X, LANE_Y, 4)));
        int remaining = Rules.INITIAL_HP / Rules.SHOT_DAMAGE - 1;
        for (int i = 0; i < remaining; i++)
            assertNull(state.shoot(SHOOTER, new ShootRequest(TARGET, 6 + i)));
        assertEquals(0, state.players.getLast().hp);
        assertEquals(SHOOTER, state.winner);
        assertEquals(Rules.MATCH_FINISHED, state.move(SHOOTER, new MoveRequest(NEAR_X, LANE_Y, 9)));
    }

    @Test
    void closedWallAndRangeBoundariesAreAuthoritative() {
        var state = room();
        double wallX = Rules.WALLS.getFirst().x(), wallY = Rules.WALLS.getFirst().minY();
        state.move(SHOOTER, new MoveRequest(wallX, wallY - 1, 2));
        state.move(TARGET, new MoveRequest(wallX, wallY, 2));
        assertEquals(Rules.BLOCKED_BY_WALL, state.shoot(SHOOTER, new ShootRequest(TARGET, 3)));
        state.move(SHOOTER, new MoveRequest(0, wallY - 1, 4));
        state.move(TARGET, new MoveRequest(Rules.SHOT_RANGE, wallY - 1, 3));
        assertNull(state.shoot(SHOOTER, new ShootRequest(TARGET, 5)));
        assertEquals(Rules.STALE_SEQUENCE, state.shoot(SHOOTER, new ShootRequest(TARGET, 5)));
        assertEquals(Rules.INVALID_MOVE, state.move(SHOOTER, new MoveRequest(Double.NaN, 0, 6)));
    }

    @Test
    void applicationStateRoundTripRetainsCombatAndSequence() throws Exception {
        var state = room();
        state.shoot(SHOOTER, new ShootRequest(TARGET, 2));
        var restored = Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(state), RoomState.class);
        assertEquals(state.view(), restored.view());
        assertEquals(Rules.STALE_SEQUENCE, restored.shoot(SHOOTER, new ShootRequest(TARGET, 2)));
        assertNull(restored.shoot(SHOOTER, new ShootRequest(TARGET, 3)));
        assertEquals(Rules.INITIAL_HP - Rules.SHOT_DAMAGE * 2, restored.players.getLast().hp);
    }

    @Test
    void matchmakingPairsByModeAndPreservesQueuedPlayersAndRoomSequence() throws Exception {
        var state = new MatchmakingState();
        var first = new QueueRequest(SHOOTER);
        state.enqueue(first);
        state.enqueue(first);
        state.enqueue(new QueueRequest(TARGET, OTHER_MODE));
        assertTrue(state.pair().isEmpty());
        var restored =
                Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(state), MatchmakingState.class);
        restored.enqueue(new QueueRequest("p3"));
        var pair = restored.pair();
        assertEquals(Rules.PLAYERS_PER_ROOM, pair.size());
        assertTrue(pair.stream().allMatch(p -> p.mode().equals(Names.DEFAULT_MODE)));
        assertEquals(Names.ROOM_PREFIX + 1, restored.allocateRoom());
        restored.remove(pair);
        restored =
                Json.MAPPER.readValue(
                        Json.MAPPER.writeValueAsBytes(restored), MatchmakingState.class);
        assertEquals(Names.ROOM_PREFIX + 2, restored.allocateRoom());
        restored.enqueue(new QueueRequest("p4", OTHER_MODE));
        assertEquals(TARGET, restored.pair().getFirst().playerId());
    }
}
