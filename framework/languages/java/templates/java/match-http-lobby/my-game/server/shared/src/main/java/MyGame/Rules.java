package MyGame;

public final class Rules {
    private Rules() {}

    public static final int PLAYERS = 2, INITIAL_HP = 100, DAMAGE = 25, FIRE_INTERVAL_MS = 250;
    public static final long MAX_SAFE_INTEGER = 9_007_199_254_740_991L;
    public static final double MAP_MIN = 0,
            MAP_MAX = 100,
            MAX_STEP = 2,
            MOVE_SPEED = 10,
            RANGE = 80,
            SPAWN_AX = 40,
            SPAWN_AY = 10,
            SPAWN_BX = 60,
            SPAWN_BY = 10,
            WALL_X = 50,
            WALL_MIN_Y = 20,
            WALL_MAX_Y = 60;
    public static final String OUT_OF_RANGE = "out-of-range",
            BLOCKED = "blocked-by-wall",
            NO_TARGET = "no-target",
            ADMISSION_REJECTED = "admission-rejected",
            INVALID_MOVE = "invalid-move",
            STEP_TOO_LARGE = "step-too-large",
            OUTSIDE_MAP = "outside-map",
            MOVE_BLOCKED_BY_WALL = "move-blocked-by-wall",
            MOVE_BUDGET = "move-budget",
            FIRE_TOO_SOON = "fire-too-soon",
            INTEGER_OUT_OF_RANGE = "integer-out-of-range",
            STALE_SEQUENCE = "stale-sequence",
            FINISHED = "match-finished",
            PACKET_NOT_ALLOWED = "packet-not-allowed";
    public static final String PLAYING = "Playing", FINISHED_PHASE = "Finished";
    public record Wall(double x, double minY, double maxY) {}
    public static final java.util.List<Wall> WALLS = java.util.List.of(new Wall(WALL_X, WALL_MIN_Y, WALL_MAX_Y));
}
