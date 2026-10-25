package MyGame.field;

import MyGame.*;

import java.util.*;
import java.util.function.LongSupplier;

public final class FieldPositionDomain {
    private final String fieldId;
    private final LongSupplier clock;
    private final Map<String, PlayerState> players = new LinkedHashMap<>();
    private final Map<String, EnterField> arrivals = new LinkedHashMap<>();

    public FieldPositionDomain(String fieldId) {
        this(fieldId, () -> System.nanoTime() / 1_000_000L);
    }

    public FieldPositionDomain(String fieldId, LongSupplier monotonicMilliseconds) {
        this.fieldId = Objects.requireNonNull(fieldId);
        this.clock = Objects.requireNonNull(monotonicMilliseconds);
    }

    public static FieldLocationResolution resolveLocation(double x) {
        if (!Double.isFinite(x))
            return new FieldLocationResolution(null, new ActionRejected(Rejection.NON_FINITE));
        if (x < 0 || x >= Rules.FIELD_WIDTH * Rules.FIELD_COUNT)
            return new FieldLocationResolution(null, new ActionRejected(Rejection.OUTSIDE_WORLD));
        return new FieldLocationResolution(Rules.fieldId(x), null);
    }

    public ActionRejected queueArrival(String actorId, EnterField arrival) {
        var location = resolveLocation(arrival.x());
        if (location.rejection() != null) return location.rejection();
        if (arrival.sequence() > Rules.MAX_SAFE_INTEGER)
            return new ActionRejected(Rejection.INTEGER_OUT_OF_RANGE);
        if (arrival.sequence() < 0)
            return new ActionRejected(Rejection.STALE_SEQUENCE);
        if (arrival.remainingMs() < 0 || arrival.remainingMs() > Rules.FULL_BUDGET_MS)
            return new ActionRejected(Rejection.INTEGER_OUT_OF_RANGE);
        if (!fieldId.equals(location.fieldId()))
            return new ActionRejected(Rejection.OUTSIDE_WORLD);
        arrivals.put(actorId, arrival);
        return null;
    }

    public EnterField takeArrival(String actorId) {
        return arrivals.remove(actorId);
    }

    public void addPlayer(String actorId, EnterField arrival) {
        long acceptedAt = clock.getAsLong() - (Rules.FULL_BUDGET_MS - arrival.remainingMs());
        players.put(actorId, new PlayerState(
                new Position(arrival.x()), arrival.sequence(), acceptedAt, true));
    }

    public MovementResult move(String actorId, MoveRequest request) {
        var player = players.get(actorId);
        if (player == null)
            return new MovementResult(false, new Position(0), 0, 0,
                    new ActionRejected(Rejection.STALE_SEQUENCE), fieldId, false);
        if (!Double.isFinite(request.delta())) return reject(actorId, Rejection.NON_FINITE);
        if (Math.abs(request.delta()) > Rules.MAX_STEP)
            return reject(actorId, Rejection.STEP_TOO_LARGE);
        if (request.sequence() > Rules.MAX_SAFE_INTEGER)
            return reject(actorId, Rejection.INTEGER_OUT_OF_RANGE);
        if (request.sequence() < 0 || player.sequence() >= Rules.MAX_SAFE_INTEGER
                || request.sequence() <= player.sequence())
            return reject(actorId, Rejection.STALE_SEQUENCE);

        double nextX = player.position().x() + request.delta();
        var location = resolveLocation(nextX);
        if (location.rejection() != null) return reject(actorId, location.rejection().reason());

        long now = clock.getAsLong();
        long elapsedMs = Math.max(0, now - player.lastAcceptedMilliseconds());
        double budget = Math.min(Rules.MAX_STEP, Rules.MAX_SPEED * elapsedMs / 1000d);
        double distance = Math.abs(request.delta());
        if (distance > budget) {
            int retryAfterMs = (int) Math.max(1,
                    Math.ceil((distance - budget) * 1000 / Rules.MAX_SPEED));
            return reject(actorId, Rejection.MOVE_BUDGET, retryAfterMs);
        }

        var position = new Position(nextX);
        String nextFieldId = location.fieldId();
        boolean relocates = !nextFieldId.equals(fieldId);
        if (!relocates)
            players.put(actorId, new PlayerState(position, request.sequence(), now, player.connected()));
        return new MovementResult(true, position, request.sequence(), Rules.FULL_BUDGET_MS,
                null, nextFieldId, relocates);
    }

    public PlayerStateView state(String actorId) {
        var player = players.get(actorId);
        if (player == null) throw new NoSuchElementException("Unknown player: " + actorId);
        return new PlayerStateView(player.position(), player.sequence(),
                remainingMs(player.lastAcceptedMilliseconds(), clock.getAsLong()), player.connected());
    }

    public PlayerStateView removePlayer(String actorId) {
        var player = players.remove(actorId);
        return player == null ? null : new PlayerStateView(player.position(), player.sequence(),
                remainingMs(player.lastAcceptedMilliseconds(), clock.getAsLong()), player.connected());
    }

    public PlayerStateView setConnected(String actorId, boolean connected) {
        var player = players.get(actorId);
        if (player == null) return null;
        players.put(actorId, new PlayerState(player.position(), player.sequence(),
                player.lastAcceptedMilliseconds(), connected));
        return state(actorId);
    }

    public FieldPositionSnapshot capture() {
        long now = clock.getAsLong();
        var playerSnapshots = new LinkedHashMap<String, FieldPlayerSnapshot>();
        for (var entry : players.entrySet()) {
            var player = entry.getValue();
            playerSnapshots.put(entry.getKey(), new FieldPlayerSnapshot(player.position(), player.sequence(),
                    remainingMs(player.lastAcceptedMilliseconds(), now), player.connected()));
        }
        return new FieldPositionSnapshot(fieldId, playerSnapshots, new LinkedHashMap<>(arrivals));
    }

    public void restore(FieldPositionSnapshot snapshot) {
        if (snapshot == null || !fieldId.equals(snapshot.fieldId()))
            throw new IllegalArgumentException("Field relocation state belongs to a different Field.");
        players.clear();
        long now = clock.getAsLong();
        for (var entry : snapshot.players().entrySet()) {
            var state = entry.getValue();
            long acceptedAt = now - (Rules.FULL_BUDGET_MS - state.remainingMs());
            players.put(entry.getKey(), new PlayerState(state.position(), state.sequence(), acceptedAt,
                    state.connected()));
        }
        arrivals.clear();
        arrivals.putAll(snapshot.arrivals());
    }

    private MovementResult reject(String actorId, String reason) {
        return reject(actorId, reason, 0);
    }

    private MovementResult reject(String actorId, String reason, int retryAfterMs) {
        var player = players.get(actorId);
        return new MovementResult(false, player.position(), player.sequence(),
                remainingMs(player.lastAcceptedMilliseconds(), clock.getAsLong()),
                new ActionRejected(reason, retryAfterMs), fieldId, false);
    }

    private static int remainingMs(long acceptedAt, long now) {
        long remaining = Rules.FULL_BUDGET_MS - Math.max(0, now - acceptedAt);
        return (int) Math.max(0, Math.min(Rules.FULL_BUDGET_MS, remaining));
    }

    private record PlayerState(Position position, long sequence, long lastAcceptedMilliseconds,
                               boolean connected) {}

    public record Position(double x) {}

    public record MovementResult(boolean accepted, Position position, long sequence, int remainingMs,
                                 ActionRejected rejection, String fieldId, boolean relocates) {}

    public record FieldLocationResolution(String fieldId, ActionRejected rejection) {}

    public record PlayerStateView(Position position, long sequence, int remainingMs, boolean connected) {}

    public record FieldPlayerSnapshot(Position position, long sequence, int remainingMs, boolean connected) {}

    public record FieldPositionSnapshot(String fieldId, Map<String, FieldPlayerSnapshot> players,
                                        Map<String, EnterField> arrivals) {}
}
