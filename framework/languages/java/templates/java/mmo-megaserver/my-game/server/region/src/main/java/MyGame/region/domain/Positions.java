package MyGame.region.domain;

import MyGame.*;
import java.util.*;
import java.util.function.LongSupplier;

public final class Positions {
    private record State(double x, long sequence, double budget, long updatedAt) {}
    private final LongSupplier clock;
    private final Map<String, State> players = new LinkedHashMap<>();

    public Positions() { this(System::nanoTime); }
    public Positions(LongSupplier clock) { this.clock = Objects.requireNonNull(clock); }

    public boolean admit(String id, int capacity) {
        if (!players.containsKey(id) && players.size() >= capacity) return false;
        players.putIfAbsent(id, new State(Names.REGION_MIN_X, 0, Names.MAX_STEP, clock.getAsLong()));
        return true;
    }
    public void leave(String id) { players.remove(id); }

    public ActionRejected move(String id, double delta, long sequence) {
        var state = Objects.requireNonNull(players.get(id), "Player is not in Region");
        if (sequence > Names.MAX_SAFE_INTEGER) return new ActionRejected("integer-out-of-range", 0);
        if (sequence <= state.sequence()) return new ActionRejected("stale-sequence", 0);
        if (!Double.isFinite(delta)) return new ActionRejected("non-finite", 0);
        var distance = Math.abs(delta);
        if (distance > Names.MAX_STEP) return new ActionRejected("step-too-large", 0);
        var next = state.x() + delta;
        if (next < Names.REGION_MIN_X || next > Names.REGION_MAX_X)
            return new ActionRejected("outside-region", 0);
        var now = clock.getAsLong();
        var available = available(state, now);
        if (distance > available)
            return new ActionRejected("move-budget", Math.max(1, (int)Math.ceil((distance - available) / Names.MAX_SPEED * 1000.0)));
        players.put(id, new State(next, sequence, available - distance, now));
        return null;
    }
    public List<PlayerPosition> view() {
        return players.entrySet().stream().sorted(Map.Entry.comparingByKey())
            .map(e -> new PlayerPosition(e.getKey(), e.getValue().x(), e.getValue().sequence())).toList();
    }
    public Set<String> playerIds() { return Collections.unmodifiableSet(players.keySet()); }
    public Map<String, Position> snapshot() {
        var now = clock.getAsLong();
        var result = new LinkedHashMap<String, Position>();
        players.entrySet().stream().sorted(Map.Entry.comparingByKey()).forEach(e -> {
            var state = e.getValue();
            var remaining = Math.max(0L, (long)Math.ceil((Names.MAX_STEP - available(state, now)) / Names.MAX_SPEED * 1000.0));
            result.put(e.getKey(), new Position(state.x(), state.sequence(), remaining));
        });
        return result;
    }
    public void restore(Map<String, Position> snapshot) {
        var now = clock.getAsLong();
        var restored = new LinkedHashMap<String, State>();
        for (var entry : Objects.requireNonNull(snapshot).entrySet()) {
            var p = entry.getValue();
            if (entry.getKey() == null || entry.getKey().isBlank() || p == null || !Double.isFinite(p.x())
                || p.x() < Names.REGION_MIN_X || p.x() > Names.REGION_MAX_X
                || p.sequence() < 0 || p.sequence() > Names.MAX_SAFE_INTEGER
                || p.remainingMs() < 0 || p.remainingMs() > Math.ceil(Names.MAX_STEP / Names.MAX_SPEED * 1000.0))
                throw new IllegalArgumentException("Invalid Region position snapshot");
            restored.put(entry.getKey(), new State(p.x(), p.sequence(), Names.MAX_STEP - p.remainingMs() * Names.MAX_SPEED / 1000.0, now));
        }
        players.clear();
        players.putAll(restored);
    }
    private double available(State state, long now) {
        var elapsed = (now - state.updatedAt()) / 1_000_000_000.0;
        return elapsed <= 0 ? state.budget() : Math.min(Names.MAX_STEP, state.budget() + elapsed * Names.MAX_SPEED);
    }
}
