package MyGame.match.domain;

import MyGame.*;

import java.util.*;

public final class RoomState {
    public List<Combatant> players = new ArrayList<>();
    public String winner;

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

    public boolean admit(String id) {
        if (players.stream().anyMatch(p -> p.playerId.equals(id))) return true;
        if (players.size() >= Rules.PLAYERS) return false;
        players.add(new Combatant(id));
        return true;
    }

    private Combatant player(String id) {
        return players.stream()
                .filter(p -> p.playerId.equals(id))
                .findFirst()
                .orElseThrow(() -> new IllegalArgumentException("Unknown room player"));
    }

    private String validate(Combatant player, long sequence) {
        if (sequence <= player.sequence) return Rules.STALE_SEQUENCE;
        player.sequence = sequence;
        return winner == null ? null : Rules.FINISHED;
    }

    public String move(String id, MoveRequest request) {
        var p = player(id);
        var reason = validate(p, request.sequence());
        if (reason != null) return reason;
        if (!Double.isFinite(request.x()) || !Double.isFinite(request.y()))
            return Rules.INVALID_MOVE;
        p.x = request.x();
        p.y = request.y();
        return null;
    }

    public String shoot(String id, ShootRequest request) {
        var p = player(id);
        var reason = validate(p, request.sequence());
        if (reason != null) return reason;
        var target =
                players.stream()
                        .filter(
                                t ->
                                        t.playerId.equals(request.targetId())
                                                && !t.playerId.equals(id)
                                                && t.hp > 0)
                        .findFirst()
                        .orElse(null);
        if (target == null) return Rules.NO_TARGET;
        double dx = target.x - p.x, dy = target.y - p.y;
        if (dx * dx + dy * dy > Rules.RANGE * Rules.RANGE) return Rules.OUT_OF_RANGE;
        boolean blocked;
        if (dx == 0)
            blocked =
                    p.x == Rules.WALL_X
                            && Math.max(p.y, target.y) >= Rules.WALL_MIN_Y
                            && Math.min(p.y, target.y) <= Rules.WALL_MAX_Y;
        else {
            double t = (Rules.WALL_X - p.x) / dx, y = p.y + t * dy;
            blocked = t >= 0 && t <= 1 && y >= Rules.WALL_MIN_Y && y <= Rules.WALL_MAX_Y;
        }
        if (blocked) return Rules.BLOCKED;
        target.hp = Math.max(0, target.hp - Rules.DAMAGE);
        if (target.hp == 0) winner = id;
        return null;
    }

    public List<PlayerView> view() {
        return players.stream().map(p -> new PlayerView(p.playerId, p.x, p.y, p.hp)).toList();
    }
}
