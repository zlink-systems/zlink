package MyGame.match;

import MyGame.*;

import java.util.*;
import java.util.function.LongSupplier;

public final class RoomState {
    private static final long MOVE_BUDGET_MS =
            Math.round(Rules.MAX_STEP * 1000.0 / Rules.MAX_SPEED);

    public record CombatantSnapshot(
            String playerId,
            double x,
            double y,
            int hp,
            long lastSequence,
            Long moveBudgetRemainingMs,
            Long fireCooldownRemainingMs) {}

    public record Snapshot(String phase, String winner, List<CombatantSnapshot> players) {}

    private static final class Combatant {
        final String playerId;
        double x;
        double y;
        int hp = Rules.INITIAL_HP;
        long lastSequence;
        Long lastMoveTimestamp;
        Long lastShotTimestamp;

        Combatant(String playerId, double x, double y) {
            this.playerId = playerId;
            this.x = x;
            this.y = y;
        }
    }

    private final LongSupplier monotonicClock;
    private final List<Combatant> players = new ArrayList<>();
    private String winner;

    public RoomState() {
        this(System::nanoTime);
    }

    RoomState(LongSupplier monotonicClock) {
        this.monotonicClock = Objects.requireNonNull(monotonicClock);
    }

    public boolean admit(String id) {
        if (players.stream().anyMatch(player -> player.playerId.equals(id))) return true;
        if (players.size() >= Rules.PLAYERS_PER_ROOM) return false;

        var spawn = players.isEmpty()
                ? new double[] {Rules.FIRST_SPAWN_X, Rules.FIRST_SPAWN_Y}
                : new double[] {Rules.SECOND_SPAWN_X, Rules.SECOND_SPAWN_Y};
        players.add(new Combatant(id, spawn[0], spawn[1]));
        return true;
    }

    private Combatant player(String id) {
        return players.stream()
                .filter(player -> player.playerId.equals(id))
                .findFirst()
                .orElseThrow(() -> new IllegalArgumentException("Unknown room player"));
    }

    private ActionRejected validate(Combatant player, long sequence) {
        if (sequence > Rules.MAX_SAFE_INTEGER)
            return new ActionRejected(Rules.INTEGER_OUT_OF_RANGE);
        if (sequence <= player.lastSequence) return new ActionRejected(Rules.STALE_SEQUENCE);
        return winner == null ? null : new ActionRejected(Rules.MATCH_FINISHED);
    }

    private static void commitAcceptedAction(Combatant player, long sequence) {
        player.lastSequence = sequence;
    }

    public ActionRejected move(String id, MoveRequest move) {
        var player = player(id);
        var rejection = validate(player, move.sequence());
        if (rejection != null) return rejection;
        if (!Double.isFinite(move.dx()) || !Double.isFinite(move.dy()))
            return new ActionRejected(Rules.INVALID_MOVE);

        var distance = Math.sqrt(move.dx() * move.dx() + move.dy() * move.dy());
        if (distance > Rules.MAX_STEP) return new ActionRejected(Rules.STEP_TOO_LARGE);

        var targetX = player.x + move.dx();
        var targetY = player.y + move.dy();
        if (targetX < Rules.MAP_MIN_X
                || targetX > Rules.MAP_MAX_X
                || targetY < Rules.MAP_MIN_Y
                || targetY > Rules.MAP_MAX_Y)
            return new ActionRejected(Rules.OUTSIDE_MAP);

        if (Rules.WALLS.stream()
                .anyMatch(wall -> intersectsWall(player.x, player.y, targetX, targetY, wall)))
            return new ActionRejected(Rules.MOVE_BLOCKED_BY_WALL);

        var now = monotonicClock.getAsLong();
        if (player.lastMoveTimestamp != null) {
            var elapsedMs = elapsedMilliseconds(player.lastMoveTimestamp, now);
            var budget = Math.min(Rules.MAX_STEP, Rules.MAX_SPEED * elapsedMs / 1000.0);
            if (distance > budget) {
                var remainingDistance = distance - budget;
                var retryAfterMs = Math.toIntExact(Math.max(
                        1L,
                        (long) Math.ceil(remainingDistance * 1000.0 / Rules.MAX_SPEED)));
                return new ActionRejected(Rules.MOVE_BUDGET, retryAfterMs);
            }
        }

        player.x = targetX;
        player.y = targetY;
        commitAcceptedAction(player, move.sequence());
        player.lastMoveTimestamp = now;
        return null;
    }

    public ActionRejected shoot(String id, ShootRequest shot) {
        var shooter = player(id);
        var rejection = validate(shooter, shot.sequence());
        if (rejection != null) return rejection;

        var target = players.stream()
                .filter(player -> player.playerId.equals(shot.targetId())
                        && !player.playerId.equals(id)
                        && player.hp > 0)
                .findFirst()
                .orElse(null);
        if (target == null) return new ActionRejected(Rules.NO_TARGET);

        var dx = target.x - shooter.x;
        var dy = target.y - shooter.y;
        if (dx * dx + dy * dy > Rules.SHOT_RANGE * Rules.SHOT_RANGE)
            return new ActionRejected(Rules.OUT_OF_RANGE);
        if (Rules.WALLS.stream()
                .anyMatch(wall -> intersectsWall(shooter.x, shooter.y, target.x, target.y, wall)))
            return new ActionRejected(Rules.BLOCKED_BY_WALL);

        var now = monotonicClock.getAsLong();
        if (shooter.lastShotTimestamp != null) {
            var remainingMs = Rules.FIRE_INTERVAL_MS
                    - elapsedMilliseconds(shooter.lastShotTimestamp, now);
            if (remainingMs > 0) {
                var retryAfterMs = Math.toIntExact((long) Math.ceil(remainingMs));
                return new ActionRejected(Rules.FIRE_TOO_SOON, retryAfterMs);
            }
        }

        target.hp = Math.max(0, target.hp - Rules.SHOT_DAMAGE);
        if (target.hp == 0) winner = id;
        commitAcceptedAction(shooter, shot.sequence());
        shooter.lastShotTimestamp = now;
        return null;
    }

    private static double elapsedMilliseconds(long startingTimestamp, long endingTimestamp) {
        return Math.max(0, endingTimestamp - startingTimestamp) / 1_000_000.0;
    }

    private static boolean intersectsWall(
            double ax, double ay, double bx, double by, Rules.Wall wall) {
        if (ax == bx)
            return ax == wall.x()
                    && Math.max(ay, by) >= wall.minY()
                    && Math.min(ay, by) <= wall.maxY();

        var t = (wall.x() - ax) / (bx - ax);
        var y = ay + t * (by - ay);
        return t >= 0 && t <= 1 && y >= wall.minY() && y <= wall.maxY();
    }

    public List<PlayerView> view() {
        return players.stream()
                .map(player -> new PlayerView(
                        player.playerId, player.x, player.y, player.hp, player.lastSequence))
                .toList();
    }

    public List<String> playerIds() {
        return players.stream().map(player -> player.playerId).toList();
    }

    public String phase() {
        return winner == null ? Rules.PLAYING_PHASE : Rules.FINISHED_PHASE;
    }

    public String winner() {
        return winner;
    }

    public Snapshot snapshot() {
        var now = monotonicClock.getAsLong();
        var snapshots = players.stream()
                .map(player -> new CombatantSnapshot(
                        player.playerId,
                        player.x,
                        player.y,
                        player.hp,
                        player.lastSequence,
                        remainingMilliseconds(player.lastMoveTimestamp, MOVE_BUDGET_MS, now),
                        remainingMilliseconds(
                                player.lastShotTimestamp, Rules.FIRE_INTERVAL_MS, now)))
                .toList();
        return new Snapshot(phase(), winner, List.copyOf(snapshots));
    }

    public void restore(Snapshot snapshot) {
        if (snapshot == null || snapshot.players() == null)
            throw new IllegalArgumentException("Missing room state snapshot");
        var expectedPhase = snapshot.winner() == null
                ? Rules.PLAYING_PHASE
                : Rules.FINISHED_PHASE;
        if (!expectedPhase.equals(snapshot.phase())
                || snapshot.players().size() > Rules.PLAYERS_PER_ROOM)
            throw new IllegalArgumentException("Invalid room state snapshot");

        var now = monotonicClock.getAsLong();
        players.clear();
        for (var player : snapshot.players()) {
            var restored = new Combatant(player.playerId(), player.x(), player.y());
            restored.hp = player.hp();
            restored.lastSequence = player.lastSequence();
            restored.lastMoveTimestamp = restoreTimestamp(
                    player.moveBudgetRemainingMs(), MOVE_BUDGET_MS, now);
            restored.lastShotTimestamp = restoreTimestamp(
                    player.fireCooldownRemainingMs(), Rules.FIRE_INTERVAL_MS, now);
            players.add(restored);
        }
        winner = snapshot.winner();
    }

    private static Long remainingMilliseconds(Long lastTimestamp, long durationMs, long now) {
        if (lastTimestamp == null) return null;
        return Math.max(
                0,
                (long) Math.ceil(durationMs - elapsedMilliseconds(lastTimestamp, now)));
    }

    private static Long restoreTimestamp(Long remainingMilliseconds, long durationMs, long now) {
        if (remainingMilliseconds == null) return null;
        var boundedRemaining = Math.max(0, Math.min(durationMs, remainingMilliseconds));
        var elapsedNanos = (long) Math.ceil((durationMs - boundedRemaining) * 1_000_000.0);
        return now - elapsedNanos;
    }
}
