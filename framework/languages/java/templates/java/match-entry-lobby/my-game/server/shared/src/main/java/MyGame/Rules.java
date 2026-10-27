package MyGame;

import java.util.List;

public final class Rules {
    private Rules() {}

    public static final int MATCHMAKING_CAPACITY = 1,
            PLAYERS_PER_ROOM = 2,
            INITIAL_HP = 100,
            SHOT_DAMAGE = 25;
    public static final long MAX_SAFE_INTEGER = 9_007_199_254_740_991L;
    public static final double SHOT_RANGE = 80,
            MAP_MIN_X = 0,
            MAP_MAX_X = 100,
            MAP_MIN_Y = 0,
            MAP_MAX_Y = 100,
            MAX_STEP = 2,
            MAX_SPEED = 10,
            FIRST_SPAWN_X = 40,
            FIRST_SPAWN_Y = 10,
            SECOND_SPAWN_X = 60,
            SECOND_SPAWN_Y = 10,
            WALL_X = 50,
            WALL_MIN_Y = 20,
            WALL_MAX_Y = 60;
    public static final int FIRE_INTERVAL_MS = 250;
    public static final String PLAYING_PHASE = "Playing", FINISHED_PHASE = "Finished";

    public record Wall(double x, double minY, double maxY) {}

    public static final List<Wall> WALLS =
            List.of(new Wall(WALL_X, WALL_MIN_Y, WALL_MAX_Y));
    public static final String ADMISSION_REJECTED = "admission-rejected",
            OUT_OF_RANGE = "out-of-range",
            BLOCKED_BY_WALL = "blocked-by-wall",
            NO_TARGET = "no-target",
            INVALID_MOVE = "invalid-move",
            STALE_SEQUENCE = "stale-sequence",
            MATCH_FINISHED = "match-finished",
            STEP_TOO_LARGE = "step-too-large",
            OUTSIDE_MAP = "outside-map",
            MOVE_BLOCKED_BY_WALL = "move-blocked-by-wall",
            MOVE_BUDGET = "move-budget",
            FIRE_TOO_SOON = "fire-too-soon",
            INTEGER_OUT_OF_RANGE = "integer-out-of-range",
            PACKET_NOT_ALLOWED = "packet-not-allowed";
}
