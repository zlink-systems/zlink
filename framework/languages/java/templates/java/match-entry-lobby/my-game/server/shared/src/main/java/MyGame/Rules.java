package MyGame;

import java.util.List;

public final class Rules {
    private Rules() {}

    public static final int MATCHMAKING_CAPACITY = 1,
            PLAYERS_PER_ROOM = 2,
            INITIAL_HP = 100,
            SHOT_DAMAGE = 25;
    public static final double SHOT_RANGE = 80;

    public record Wall(double x, double minY, double maxY) {}

    public static final List<Wall> WALLS = List.of(new Wall(50, 0, 100));
    public static final String ADMISSION_REJECTED = "admission-rejected",
            OUT_OF_RANGE = "out-of-range",
            BLOCKED_BY_WALL = "blocked-by-wall",
            NO_TARGET = "no-target",
            INVALID_MOVE = "invalid-move",
            STALE_SEQUENCE = "stale-sequence",
            MATCH_FINISHED = "match-finished";
}
