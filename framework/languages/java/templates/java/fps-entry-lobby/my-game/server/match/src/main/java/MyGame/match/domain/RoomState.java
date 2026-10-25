package MyGame.match.domain;

import MyGame.*;
import MyGame.Geometry.Vec3;
import MyGame.Geometry.ArenaMap;

import java.util.ArrayList;
import java.util.List;
import java.util.function.DoubleSupplier;

public final class RoomState {
    private final DoubleSupplier nowMs;
    private final List<Combatant> players = new ArrayList<>();
    private MatchPhase phase = MatchPhase.Waiting;
    private String winner;

    public RoomState() {
        this(null);
    }

    public RoomState(DoubleSupplier clock) {
        nowMs =
                clock != null
                        ? clock
                        : () -> System.nanoTime() / 1_000_000.0;
    }

    public List<Combatant> players() {
        return List.copyOf(players);
    }

    public MatchPhase phase() {
        return phase;
    }

    public String winner() {
        return winner;
    }

    public boolean admit(String playerId) {
        if (players.stream().anyMatch(player -> player.playerId.equals(playerId))) return true;
        if (players.size() >= Rules.PLAYERS_PER_ROOM) return false;
        players.add(new Combatant(playerId, ArenaMap.SPAWNS.get(players.size())));
        if (players.size() == Rules.PLAYERS_PER_ROOM) phase = MatchPhase.Playing;
        return true;
    }

    private Combatant player(String id) {
        return players.stream()
                .filter(player -> player.playerId.equals(id))
                .findFirst()
                .orElseThrow(() -> new IllegalArgumentException("Unknown room player"));
    }

    private String validateAction(Combatant player, long sequence) {
        if (sequence <= player.sequence) return Rejection.STALE_SEQUENCE;
        if (phase == MatchPhase.Finished) return Rejection.MATCH_FINISHED;
        return null;
    }

    public ActionRejected move(String id, MoveRequest move) {
        var player = player(id);
        var reason = validateAction(player, move.sequence());
        if (reason != null) return new ActionRejected(reason);
        if (!move.delta().isFinite()) return new ActionRejected(Rejection.NON_FINITE);
        if (move.delta().y() != 0) return new ActionRejected(Rejection.VERTICAL_MOVE);
        double distance = move.delta().length();
        if (distance > Rules.MAX_STEP) return new ActionRejected(Rejection.STEP_TOO_LARGE);
        var next = player.position.plus(move.delta());
        if (!ArenaMap.BOUNDS.contains(next)) return new ActionRejected(Rejection.OUTSIDE_MAP);
        if (ArenaMap.WALLS.stream().anyMatch(wall -> wall.intersect(player.position, move.delta(), 1) != null))
            return new ActionRejected(Rejection.MOVE_BLOCKED_BY_WALL);

        double now = nowMs.getAsDouble();
        if (player.lastMoveMs != null) {
            double elapsed = Math.max(0, now - player.lastMoveMs);
            double budget = Math.min(Rules.MAX_STEP, Rules.MAX_SPEED * elapsed / 1000);
            if (distance > budget) {
                int retryAfterMs = Math.max(1, (int) Math.ceil(distance * 1000 / Rules.MAX_SPEED - elapsed));
                return new ActionRejected(Rejection.MOVE_BUDGET, retryAfterMs);
            }
        }
        player.position = next;
        player.sequence = move.sequence();
        player.lastMoveMs = now;
        return null;
    }

    public ShotDecision shoot(String id, ShootRequest shot) {
        var shooter = player(id);
        var reason = validateAction(shooter, shot.sequence());
        if (reason != null) return new ShotDecision(new ShotRejected(reason), null);
        var direction = shot.aimDirection().tryNormalize().orElse(null);
        if (direction == null)
            return new ShotDecision(new ShotRejected(Rejection.INVALID_AIM), null);

        double now = nowMs.getAsDouble();
        if (shooter.nextFireMs != null && now < shooter.nextFireMs) {
            int retryAfterMs = Math.max(1, (int) Math.ceil(shooter.nextFireMs - now));
            return new ShotDecision(new ShotRejected(Rejection.FIRE_TOO_SOON, retryAfterMs), null);
        }
        var origin = shooter.position.plus(new Vec3(0, Rules.EYE_HEIGHT, 0));
        double wallDistance = Double.POSITIVE_INFINITY;
        for (var wall : ArenaMap.WALLS) {
            var distance = wall.intersect(origin, direction, Rules.SHOT_RANGE);
            if (distance != null) wallDistance = Math.min(wallDistance, distance);
        }
        Combatant target = null;
        double targetDistance = Double.POSITIVE_INFINITY;
        for (var candidate : players) {
            if (candidate == shooter || candidate.hp == 0) continue;
            Double distance = ArenaMap.playerBox(candidate.position).intersect(
                    origin, direction, Rules.SHOT_RANGE);
            if (distance != null && distance < targetDistance) {
                target = candidate;
                targetDistance = distance;
            }
        }

        String outcome = ShotOutcome.MISS;
        if (wallDistance <= targetDistance && Double.isFinite(wallDistance)) {
            outcome = ShotOutcome.BLOCKED_BY_WALL;
        } else if (target != null) {
            outcome = ShotOutcome.HIT;
            target.hp = Math.max(0, target.hp - Rules.SHOT_DAMAGE);
            if (target.hp == 0) {
                phase = MatchPhase.Finished;
                winner = id;
            }
        }
        shooter.sequence = shot.sequence();
        shooter.nextFireMs = now + Rules.FIRE_INTERVAL_MS;
        return new ShotDecision(
                null,
                new ShotResult(outcome, outcome.equals(ShotOutcome.HIT) ? target.playerId : null, shot.sequence()));
    }

    public List<PlayerView> view() {
        return players.stream()
                .map(player -> new PlayerView(player.playerId, player.position, player.hp, player.sequence))
                .toList();
    }

    public RoomSnapshot capture() {
        double now = nowMs.getAsDouble();
        var snapshots =
                players.stream()
                        .map(
                                player ->
                                        new CombatantSnapshot(
                                                player.playerId,
                                                player.position,
                                                player.hp,
                                                player.sequence,
                                                player.nextFireMs == null
                                                        ? 0
                                                        : Math.max(0, player.nextFireMs - now),
                                                player.lastMoveMs == null
                                                        ? 0
                                                        : Math.max(
                                                                0,
                                                                Rules.MAX_STEP * 1000 / Rules.MAX_SPEED
                                                                        - (now - player.lastMoveMs))))
                        .toList();
        return new RoomSnapshot(snapshots, phase, winner);
    }

    public void restore(RoomSnapshot snapshot) {
        double now = nowMs.getAsDouble();
        players.clear();
        for (var player : snapshot.players()) {
            var restored = new Combatant(player.playerId(), player.position());
            restored.hp = player.hp();
            restored.sequence = player.sequence();
            restored.nextFireMs = now + player.fireRemainingMs();
            restored.lastMoveMs =
                    player.moveRemainingMs() > 0
                            ? now
                                    - (Rules.MAX_STEP * 1000 / Rules.MAX_SPEED
                                            - player.moveRemainingMs())
                            : null;
            players.add(restored);
        }
        phase = snapshot.phase();
        winner = snapshot.winner();
    }

    public static final class Combatant {
        private final String playerId;
        private Vec3 position;
        private int hp = Rules.INITIAL_HP;
        private long sequence;
        private Double lastMoveMs;
        private Double nextFireMs;

        private Combatant(String playerId, Vec3 position) {
            this.playerId = playerId;
            this.position = position;
        }

        public String playerId() {
            return playerId;
        }

        public Vec3 position() {
            return position;
        }

        public int hp() {
            return hp;
        }

        public long sequence() {
            return sequence;
        }
    }

    public record ShotDecision(ShotRejected rejected, ShotResult result) {}

    public record CombatantSnapshot(
            String playerId,
            Vec3 position,
            int hp,
            long sequence,
            double fireRemainingMs,
            double moveRemainingMs) {}

    public record RoomSnapshot(List<CombatantSnapshot> players, MatchPhase phase, String winner) {}
}
