package MyGame.match;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import com.fasterxml.jackson.databind.JsonNode;
import java.nio.charset.StandardCharsets;
import java.util.List;
import org.junit.jupiter.api.Test;

final class DomainTest {
    private static final String SHOOTER = "p1", TARGET = "p2", OTHER_MODE = "squad";
    private static final double WALL_CENTER_Y = (Rules.WALL_MIN_Y + Rules.WALL_MAX_Y) / 2;

    private static final class ManualClock {
        private long timestamp;

        ManualClock(long timestamp) {
            this.timestamp = timestamp;
        }

        long timestamp() {
            return timestamp;
        }

        void advanceMillis(long milliseconds) {
            timestamp += milliseconds * 1_000_000L;
        }

        void advanceSeconds(long seconds) {
            advanceMillis(seconds * 1000);
        }
    }

    private static RoomState room(ManualClock clock) {
        var state = new RoomState(clock::timestamp);
        assertTrue(state.admit(SHOOTER));
        assertTrue(state.admit(TARGET));
        assertTrue(state.admit(SHOOTER));
        assertFalse(state.admit("p3"));
        return state;
    }

    private static RoomState positioned(
            ManualClock clock,
            double shooterX,
            double shooterY,
            double targetX,
            double targetY) {
        var state = room(clock);
        var snapshot = state.snapshot();
        var players = List.of(
                position(snapshot.players().get(0), shooterX, shooterY),
                position(snapshot.players().get(1), targetX, targetY));
        state.restore(new RoomState.Snapshot(snapshot.phase(), snapshot.winner(), players));
        return state;
    }

    private static RoomState.CombatantSnapshot position(
            RoomState.CombatantSnapshot player, double x, double y) {
        return new RoomState.CombatantSnapshot(
                player.playerId(),
                x,
                y,
                player.hp(),
                player.lastSequence(),
                player.moveBudgetRemainingMs(),
                player.fireCooldownRemainingMs());
    }

    private static PlayerView player(RoomState room, String playerId) {
        return room.view().stream()
                .filter(view -> view.playerId().equals(playerId))
                .findFirst()
                .orElseThrow();
    }

    private static void rejectedWithoutStateChange(
            RoomState room, ActionRejected expected, java.util.function.Supplier<ActionRejected> action) {
        var before = room.snapshot();
        assertEquals(expected, action.get());
        assertEquals(before, room.snapshot());
    }

    @Test
    void sequenceIsSharedAcrossActionsAndRejectionsDoNotAdvanceIt() {
        var state = room(new ManualClock(0));
        assertEquals(40, player(state, SHOOTER).x());
        assertEquals(10, player(state, SHOOTER).y());
        assertEquals(60, player(state, TARGET).x());
        assertEquals(10, player(state, TARGET).y());

        assertNull(state.move(SHOOTER, new MoveRequest(0, 0, 1)));
        rejectedWithoutStateChange(
                state,
                new ActionRejected(Rules.STALE_SEQUENCE),
                () -> state.move(SHOOTER, new MoveRequest(0, 0, 1)));
        rejectedWithoutStateChange(
                state,
                new ActionRejected(Rules.STALE_SEQUENCE),
                () -> state.shoot(SHOOTER, new ShootRequest(TARGET, 0)));
        rejectedWithoutStateChange(
                state,
                new ActionRejected(Rules.INTEGER_OUT_OF_RANGE),
                () -> state.move(
                        SHOOTER,
                        new MoveRequest(0, 0, Rules.MAX_SAFE_INTEGER + 1)));

        assertNull(state.shoot(SHOOTER, new ShootRequest(TARGET, Rules.MAX_SAFE_INTEGER)));
        assertEquals(Rules.MAX_SAFE_INTEGER, player(state, SHOOTER).lastSequence());
        rejectedWithoutStateChange(
                state,
                new ActionRejected(Rules.INTEGER_OUT_OF_RANGE),
                () -> state.shoot(
                        SHOOTER,
                        new ShootRequest(TARGET, Rules.MAX_SAFE_INTEGER + 1)));

        var snapshot = state.snapshot();
        var shooter = snapshot.players().getFirst();
        var invalidSequence = Rules.MAX_SAFE_INTEGER + 1;
        state.restore(new RoomState.Snapshot(
                snapshot.phase(),
                snapshot.winner(),
                List.of(
                        new RoomState.CombatantSnapshot(
                                shooter.playerId(),
                                shooter.x(),
                                shooter.y(),
                                shooter.hp(),
                                invalidSequence,
                                shooter.moveBudgetRemainingMs(),
                                shooter.fireCooldownRemainingMs()),
                        snapshot.players().get(1))));
        rejectedWithoutStateChange(
                state,
                new ActionRejected(Rules.INTEGER_OUT_OF_RANGE),
                () -> state.move(SHOOTER, new MoveRequest(0, 0, invalidSequence)));
    }

    @Test
    void maxStepAndMapBoundsAreInclusive() {
        var state = room(new ManualClock(0));
        assertNull(state.move(SHOOTER, new MoveRequest(Rules.MAX_STEP, 0, 1)));
        assertEquals(42, player(state, SHOOTER).x());
        rejectedWithoutStateChange(
                state,
                new ActionRejected(Rules.STEP_TOO_LARGE),
                () -> state.move(SHOOTER, new MoveRequest(Rules.MAX_STEP + 0.001, 0, 2)));

        var edges = List.of(
                new double[] {2, 50, -2, 0, 0, 50},
                new double[] {98, 50, 2, 0, 100, 50},
                new double[] {50, 2, 0, -2, 50, 0},
                new double[] {50, 98, 0, 2, 50, 100});
        for (var edge : edges) {
            var edgeRoom = positioned(new ManualClock(0), edge[0], edge[1], 70, 10);
            assertNull(edgeRoom.move(SHOOTER, new MoveRequest(edge[2], edge[3], 1)));
            assertEquals(edge[4], player(edgeRoom, SHOOTER).x());
            assertEquals(edge[5], player(edgeRoom, SHOOTER).y());
        }

        var outsideCases = List.of(
                new double[] {0, 50, -0.01, 0},
                new double[] {100, 50, 0.01, 0},
                new double[] {50, 0, 0, -0.01},
                new double[] {50, 100, 0, 0.01});
        for (var outside : outsideCases) {
            var outsideRoom = positioned(new ManualClock(0), outside[0], outside[1], 70, 10);
            rejectedWithoutStateChange(
                    outsideRoom,
                    new ActionRejected(Rules.OUTSIDE_MAP),
                    () -> outsideRoom.move(
                            SHOOTER,
                            new MoveRequest(outside[2], outside[3], 1)));
        }
    }

    @Test
    void movementRejectsWallEndpointContactAndCrossing() {
        for (var endpointY : new double[] {Rules.WALL_MIN_Y, Rules.WALL_MAX_Y}) {
            var state = positioned(
                    new ManualClock(0),
                    Rules.WALL_X - Rules.MAX_STEP,
                    endpointY,
                    70,
                    10);
            rejectedWithoutStateChange(
                    state,
                    new ActionRejected(Rules.MOVE_BLOCKED_BY_WALL),
                    () -> state.move(SHOOTER, new MoveRequest(Rules.MAX_STEP, 0, 1)));
        }

        var crossing = positioned(
                new ManualClock(0), Rules.WALL_X - 1, WALL_CENTER_Y, 70, 10);
        rejectedWithoutStateChange(
                crossing,
                new ActionRejected(Rules.MOVE_BLOCKED_BY_WALL),
                () -> crossing.move(SHOOTER, new MoveRequest(Rules.MAX_STEP, 0, 1)));

        var clearLane = positioned(
                new ManualClock(0),
                Rules.WALL_X - Rules.MAX_STEP,
                Rules.WALL_MIN_Y - 1,
                70,
                10);
        assertNull(clearLane.move(SHOOTER, new MoveRequest(Rules.MAX_STEP, 0, 1)));
    }

    @Test
    void movementBudgetGrantsFirstMoveThenElapsedSpeedAndCapsAtMaxStep() {
        var clock = new ManualClock(0);
        var state = room(clock);
        assertNull(state.move(SHOOTER, new MoveRequest(Rules.MAX_STEP, 0, 1)));

        clock.advanceMillis(100);
        assertNull(state.move(SHOOTER, new MoveRequest(1, 0, 2)));
        var beforeRetry = state.snapshot();
        assertEquals(
                new ActionRejected(Rules.MOVE_BUDGET, 10),
                state.move(SHOOTER, new MoveRequest(0.1, 0, 3)));
        assertEquals(beforeRetry, state.snapshot());

        clock.advanceMillis(10);
        assertNull(state.move(SHOOTER, new MoveRequest(0.1, 0, 3)));
        assertEquals(43.1, player(state, SHOOTER).x(), 0.000001);

        clock.advanceSeconds(1);
        assertNull(state.move(SHOOTER, new MoveRequest(Rules.MAX_STEP, 0, 4)));
        assertEquals(45.1, player(state, SHOOTER).x(), 0.000001);
    }

    @Test
    void finiteMovementIsRequiredAndInvalidInputDoesNotChangeState() {
        for (var value : new double[] {Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY}) {
            var state = room(new ManualClock(0));
            rejectedWithoutStateChange(
                    state,
                    new ActionRejected(Rules.INVALID_MOVE),
                    () -> state.move(SHOOTER, new MoveRequest(value, 0, 1)));
        }
    }

    @Test
    void fireCooldownReportsExactRetryAndPreservesStateOnRejection() {
        var clock = new ManualClock(0);
        var state = room(clock);
        assertNull(state.shoot(SHOOTER, new ShootRequest(TARGET, 1)));
        assertEquals(Rules.INITIAL_HP - Rules.SHOT_DAMAGE, player(state, TARGET).hp());

        rejectedWithoutStateChange(
                state,
                new ActionRejected(Rules.FIRE_TOO_SOON, Rules.FIRE_INTERVAL_MS),
                () -> state.shoot(SHOOTER, new ShootRequest(TARGET, 2)));
        clock.advanceMillis(Rules.FIRE_INTERVAL_MS - 1);
        rejectedWithoutStateChange(
                state,
                new ActionRejected(Rules.FIRE_TOO_SOON, 1),
                () -> state.shoot(SHOOTER, new ShootRequest(TARGET, 2)));

        clock.advanceMillis(1);
        assertNull(state.shoot(SHOOTER, new ShootRequest(TARGET, 2)));
        assertEquals(Rules.INITIAL_HP - 2 * Rules.SHOT_DAMAGE, player(state, TARGET).hp());
    }

    @Test
    void shotRangeWallAndMissingTargetRejectWithoutChangingState() {
        var rangeBoundary = positioned(new ManualClock(0), 10, 10, 90, 10);
        assertNull(rangeBoundary.shoot(SHOOTER, new ShootRequest(TARGET, 1)));

        for (var wallY : new double[] {Rules.WALL_MIN_Y, WALL_CENTER_Y, Rules.WALL_MAX_Y}) {
            var blocked = positioned(
                    new ManualClock(0),
                    Rules.WALL_X - 2,
                    wallY,
                    Rules.WALL_X + 2,
                    wallY);
            rejectedWithoutStateChange(
                    blocked,
                    new ActionRejected(Rules.BLOCKED_BY_WALL),
                    () -> blocked.shoot(SHOOTER, new ShootRequest(TARGET, 1)));
        }

        var far = positioned(new ManualClock(0), 10, 10, 100, 90);
        rejectedWithoutStateChange(
                far,
                new ActionRejected(Rules.OUT_OF_RANGE),
                () -> far.shoot(SHOOTER, new ShootRequest(TARGET, 1)));
        rejectedWithoutStateChange(
                far,
                new ActionRejected(Rules.NO_TARGET),
                () -> far.shoot(SHOOTER, new ShootRequest("missing-player", 1)));
    }

    @Test
    void snapshotStoresRemainingMillisecondsAndRestoresAgainstAnotherClockOrigin() throws Exception {
        var sourceClock = new ManualClock(5_000_000_000_000L);
        var source = room(sourceClock);
        assertNull(source.move(SHOOTER, new MoveRequest(Rules.MAX_STEP, 0, 1)));
        assertNull(source.shoot(SHOOTER, new ShootRequest(TARGET, 2)));
        sourceClock.advanceMillis(60);

        var snapshot = source.snapshot();
        assertEquals(140, snapshot.players().getFirst().moveBudgetRemainingMs());
        assertEquals(190, snapshot.players().getFirst().fireCooldownRemainingMs());
        var bytes = Json.MAPPER.writeValueAsBytes(snapshot);
        JsonNode json = Json.MAPPER.readTree(bytes);
        var player = json.path("players").get(0);
        assertEquals(140, player.path("moveBudgetRemainingMs").asLong());
        assertEquals(190, player.path("fireCooldownRemainingMs").asLong());
        assertFalse(new String(bytes, StandardCharsets.UTF_8).contains("Timestamp"));
        assertArrayEquals(bytes, Json.MAPPER.writeValueAsBytes(source.snapshot()));

        var destinationClock = new ManualClock(900_000_000_000_000L);
        var destination = new RoomState(destinationClock::timestamp);
        destination.restore(Json.MAPPER.readValue(bytes, RoomState.Snapshot.class));
        assertEquals(source.view(), destination.view());
        assertEquals(source.phase(), destination.phase());
        assertEquals(2, player(destination, SHOOTER).lastSequence());
        rejectedWithoutStateChange(
                destination,
                new ActionRejected(Rules.MOVE_BUDGET, 140),
                () -> destination.move(SHOOTER, new MoveRequest(Rules.MAX_STEP, 0, 3)));
        rejectedWithoutStateChange(
                destination,
                new ActionRejected(Rules.FIRE_TOO_SOON, 190),
                () -> destination.shoot(SHOOTER, new ShootRequest(TARGET, 3)));

        destinationClock.advanceMillis(140);
        assertNull(destination.move(SHOOTER, new MoveRequest(Rules.MAX_STEP, 0, 3)));
        destinationClock.advanceMillis(50);
        assertNull(destination.shoot(SHOOTER, new ShootRequest(TARGET, 4)));
    }

    @Test
    void finishedMatchRejectsMoveAndShootWithoutChangingSnapshot() {
        var clock = new ManualClock(0);
        var state = room(clock);
        for (var sequence = 1L; sequence <= Rules.INITIAL_HP / Rules.SHOT_DAMAGE; sequence++) {
            assertNull(state.shoot(SHOOTER, new ShootRequest(TARGET, sequence)));
            if (sequence < Rules.INITIAL_HP / Rules.SHOT_DAMAGE)
                clock.advanceMillis(Rules.FIRE_INTERVAL_MS);
        }

        assertEquals(Rules.FINISHED_PHASE, state.phase());
        assertEquals(SHOOTER, state.winner());
        assertEquals(0, player(state, TARGET).hp());
        rejectedWithoutStateChange(
                state,
                new ActionRejected(Rules.MATCH_FINISHED),
                () -> state.move(SHOOTER, new MoveRequest(0, 0, 5)));
        rejectedWithoutStateChange(
                state,
                new ActionRejected(Rules.MATCH_FINISHED),
                () -> state.shoot(SHOOTER, new ShootRequest(TARGET, 5)));
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
