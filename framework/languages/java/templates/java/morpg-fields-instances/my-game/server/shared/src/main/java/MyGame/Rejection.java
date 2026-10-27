package MyGame;

public final class Rejection {
    private Rejection() {}

    public static final String INVALID_PARTY = "invalid-party",
            NOT_LEADER = "not-leader",
            RUN_MISMATCH = "run-mismatch",
            STALE_SEQUENCE = "stale-sequence",
            NON_FINITE = "non-finite",
            OUTSIDE_WORLD = "outside-world",
            INTEGER_OUT_OF_RANGE = "integer-out-of-range",
            STEP_TOO_LARGE = "step-too-large",
            MOVE_BUDGET = "move-budget",
            RUN_NOT_ACTIVE = "run-not-active",
            PACKET_NOT_ALLOWED = "packet-not-allowed",
            RUN_IN_PROGRESS = "run-in-progress";
}
