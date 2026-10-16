package MyGame;

public final class Rules {
    private Rules() {}

    public static final int PLAYERS = 2, INITIAL_HP = 100, DAMAGE = 25;
    public static final double RANGE = 80, WALL_X = 50, WALL_MIN_Y = 0, WALL_MAX_Y = 100;
    public static final String OUT_OF_RANGE = "out-of-range",
            BLOCKED = "blocked-by-wall",
            NO_TARGET = "no-target",
            ADMISSION_REJECTED = "admission-rejected",
            INVALID_MOVE = "invalid-move",
            STALE_SEQUENCE = "stale-sequence",
            FINISHED = "match-finished";
}
