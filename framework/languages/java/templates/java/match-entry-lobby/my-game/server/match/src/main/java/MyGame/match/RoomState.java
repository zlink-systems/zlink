package MyGame.match;

import MyGame.*;

import java.util.*;

public final class RoomState {
    public static final class Combatant {
        public String playerId;
        public double x, y;
        public int hp = Rules.INITIAL_HP;
        public long sequence;

        public Combatant() {}

        Combatant(String id) {
            playerId = id;
        }
    }

    public List<Combatant> players = new ArrayList<>();
    public String winner;

    public boolean admit(String id) {
        if (players.stream().anyMatch(p -> p.playerId.equals(id))) return true;
        if (players.size() >= Rules.PLAYERS_PER_ROOM) return false;
        players.add(new Combatant(id));
        return true;
    }

    private Combatant player(String id) {
        for (var player : players) if (player.playerId.equals(id)) return player;
        throw new IllegalArgumentException("Unknown room player");
    }

    private String validate(Combatant player, long sequence) {
        if (sequence <= player.sequence) return Rules.STALE_SEQUENCE;
        player.sequence = sequence;
        return winner == null ? null : Rules.MATCH_FINISHED;
    }

    public String move(String id, MoveRequest request) {
        var player = player(id);
        var reason = validate(player, request.sequence());
        if (reason != null) return reason;
        if (!Double.isFinite(request.x()) || !Double.isFinite(request.y()))
            return Rules.INVALID_MOVE;
        player.x = request.x();
        player.y = request.y();
        return null;
    }

    public String shoot(String id, ShootRequest request) {
        var shooter = player(id);
        var reason = validate(shooter, request.sequence());
        if (reason != null) return reason;
        Combatant target = null;
        for (var player : players) {
            if (player.playerId.equals(request.targetId())
                    && !player.playerId.equals(id)
                    && player.hp > 0) {
                target = player;
                break;
            }
        }
        if (target == null) return Rules.NO_TARGET;
        double dx = target.x - shooter.x, dy = target.y - shooter.y;
        if (dx * dx + dy * dy > Rules.SHOT_RANGE * Rules.SHOT_RANGE) return Rules.OUT_OF_RANGE;
        for (var wall : Rules.WALLS) {
            // Touching any part of the closed wall segment blocks the shot.
            if (dx == 0) {
                if (shooter.x == wall.x()
                        && Math.max(shooter.y, target.y) >= wall.minY()
                        && Math.min(shooter.y, target.y) <= wall.maxY())
                    return Rules.BLOCKED_BY_WALL;
            } else {
                double t = (wall.x() - shooter.x) / dx, y = shooter.y + t * dy;
                if (t >= 0 && t <= 1 && y >= wall.minY() && y <= wall.maxY())
                    return Rules.BLOCKED_BY_WALL;
            }
        }
        target.hp = Math.max(0, target.hp - Rules.SHOT_DAMAGE);
        if (target.hp == 0) winner = id;
        return null;
    }

    public List<PlayerView> view() {
        return players.stream().map(p -> new PlayerView(p.playerId, p.x, p.y, p.hp)).toList();
    }
}
