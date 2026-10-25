package MyGame.zone;

import MyGame.*;

import java.util.*;
import java.util.function.LongSupplier;

public final class ZonePositionDomain {
    private final String zoneId;
    private final LongSupplier monotonicMilliseconds;
    private final Map<String, PlayerState> players = new LinkedHashMap<>();
    private final Map<String, EnterZone> arrivals = new LinkedHashMap<>();

    public ZonePositionDomain(String zoneId) {
        this(zoneId, () -> System.nanoTime() / 1_000_000L);
    }

    public ZonePositionDomain(String zoneId, LongSupplier monotonicMilliseconds) {
        this.zoneId = Objects.requireNonNull(zoneId);
        this.monotonicMilliseconds = Objects.requireNonNull(monotonicMilliseconds);
    }

    public static ZoneLocationResolution resolveLocation(double x) {
        if (!Double.isFinite(x)) return new ZoneLocationResolution(null, new ActionRejected("non-finite", 0));
        if (x < 0 || x >= (double) Rules.ZONE_WIDTH * Rules.ZONE_COUNT)
            return new ZoneLocationResolution(null, new ActionRejected("outside-world", 0));
        return new ZoneLocationResolution(Rules.zoneId(x), null);
    }

    public ActionRejected queueArrival(String playerId, EnterZone arrival) {
        var location = resolveLocation(arrival.position().x());
        if (location.rejection() != null) return location.rejection();
        if (arrival.sequence() > Rules.MAX_SAFE_INTEGER)
            return new ActionRejected("integer-out-of-range", 0);
        if (arrival.sequence() < 0) return new ActionRejected("stale-sequence", 0);
        if (arrival.remainingMs() < 0 || arrival.remainingMs() > Rules.FULL_BUDGET_AFTER_MS)
            return new ActionRejected("integer-out-of-range", 0);
        if (!zoneId.equals(location.zoneId())) return new ActionRejected("outside-world", 0);
        if (arrivals.putIfAbsent(playerId, arrival) != null)
            throw new IllegalStateException("Duplicate Zone arrival");
        return null;
    }

    public EnterZone takeArrival(String playerId) {
        return arrivals.remove(playerId);
    }

    public void addPlayer(String playerId, EnterZone arrival) {
        addPlayer(playerId, arrival, true);
    }

    public void addPlayer(String playerId, EnterZone arrival, boolean connected) {
        long acceptedAt = now() - (Rules.FULL_BUDGET_AFTER_MS - arrival.remainingMs());
        players.put(playerId, new PlayerState(arrival.position(), arrival.sequence(), acceptedAt, connected));
    }

    public MovementResult move(String playerId, MoveRequest request) {
        var player = players.get(playerId);
        if (player == null)
            return new MovementResult(false, new Position(0), 0, 0,
                    new ActionRejected("stale-sequence", 0), zoneId, false);
        if (!Double.isFinite(request.delta())) return reject(playerId, "non-finite");
        if (Math.abs(request.delta()) > Rules.MAX_STEP) return reject(playerId, "step-too-large");
        if (request.sequence() > Rules.MAX_SAFE_INTEGER) return reject(playerId, "integer-out-of-range");
        if (request.sequence() < 0 || player.sequence() >= Rules.MAX_SAFE_INTEGER
                || request.sequence() <= player.sequence()) return reject(playerId, "stale-sequence");

        var position = new Position(player.position().x() + request.delta());
        var location = resolveLocation(position.x());
        if (location.rejection() != null) return reject(playerId, location.rejection().reason());

        long now = now();
        double elapsedMilliseconds = Math.max(0, now - player.lastAcceptedMilliseconds());
        double budget = Math.min(Rules.MAX_STEP, Rules.MAX_SPEED * elapsedMilliseconds / 1000d);
        double distance = Math.abs(request.delta());
        if (distance > budget) {
            int retryAfterMs = (int) Math.max(1, Math.ceil((distance - budget) * 1000d / Rules.MAX_SPEED));
            return reject(playerId, "move-budget", retryAfterMs);
        }

        boolean relocates = !zoneId.equals(location.zoneId());
        if (!relocates)
            players.put(playerId, new PlayerState(position, request.sequence(), now, player.connected()));
        return new MovementResult(true, position, request.sequence(), Rules.FULL_BUDGET_AFTER_MS,
                null, location.zoneId(), relocates);
    }

    public PlayerStateView state(String playerId) {
        var player = players.get(playerId);
        return player == null ? null : view(player, now());
    }

    public Map<String, PlayerStateView> connectedPlayers() {
        long now = now();
        var connected = new LinkedHashMap<String, PlayerStateView>();
        players.forEach((id, player) -> {
            if (player.connected()) connected.put(id, view(player, now));
        });
        return connected;
    }

    public PlayerStateView setConnected(String playerId, boolean connected) {
        var player = players.get(playerId);
        if (player == null) return null;
        players.put(playerId, new PlayerState(player.position(), player.sequence(),
                player.lastAcceptedMilliseconds(), connected));
        return view(players.get(playerId), now());
    }

    public PlayerStateView removePlayer(String playerId) {
        var player = players.remove(playerId);
        return player == null ? null : view(player, now());
    }

    public ZonePositionSnapshot capture() {
        long now = now();
        var snapshots = new LinkedHashMap<String, ZonePlayerSnapshot>();
        players.forEach((id, player) -> snapshots.put(id, new ZonePlayerSnapshot(
                player.position(), player.sequence(), remainingMs(player.lastAcceptedMilliseconds(), now),
                player.connected())));
        return new ZonePositionSnapshot(zoneId, snapshots, new LinkedHashMap<>(arrivals));
    }

    public void restore(ZonePositionSnapshot snapshot) {
        if (!zoneId.equals(snapshot.zoneId()))
            throw new IllegalArgumentException("Zone relocation state belongs to a different Zone");
        players.clear();
        long now = now();
        snapshot.players().forEach((id, player) -> players.put(id, new PlayerState(
                player.position(), player.sequence(), now - (Rules.FULL_BUDGET_AFTER_MS - player.remainingMs()),
                player.connected())));
        arrivals.clear();
        arrivals.putAll(snapshot.arrivals());
    }

    private MovementResult reject(String playerId, String reason) {
        return reject(playerId, reason, 0);
    }

    private MovementResult reject(String playerId, String reason, int retryAfterMs) {
        var player = players.get(playerId);
        return new MovementResult(false, player.position(), player.sequence(),
                remainingMs(player.lastAcceptedMilliseconds(), now()),
                new ActionRejected(reason, retryAfterMs), zoneId, false);
    }

    private PlayerStateView view(PlayerState player, long now) {
        return new PlayerStateView(player.position(), player.sequence(),
                remainingMs(player.lastAcceptedMilliseconds(), now), player.connected());
    }

    private static int remainingMs(long acceptedAt, long now) {
        long remaining = Rules.FULL_BUDGET_AFTER_MS - Math.max(0, now - acceptedAt);
        return (int) Math.max(0, Math.min(Rules.FULL_BUDGET_AFTER_MS, remaining));
    }

    private long now() {
        return monotonicMilliseconds.getAsLong();
    }

    private record PlayerState(Position position, long sequence, long lastAcceptedMilliseconds,
            boolean connected) {}

    public record MovementResult(boolean accepted, Position position, long sequence, int remainingMs,
            ActionRejected rejection, String zoneId, boolean relocates) {}

    public record ZoneLocationResolution(String zoneId, ActionRejected rejection) {}

    public record PlayerStateView(Position position, long sequence, int remainingMs,
            boolean connected) {}

    public record ZonePlayerSnapshot(Position position, long sequence, int remainingMs,
            boolean connected) {}

    public record ZonePositionSnapshot(String zoneId, Map<String, ZonePlayerSnapshot> players,
            Map<String, EnterZone> arrivals) {}
}
