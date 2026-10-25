package MyGame;

public final class Rules {
    private Rules() {}

    public static final int MAX_MEMBERS = 4;
    public static final long MAXIMUM_INTEGER = 9_007_199_254_740_991L;
    public static final int FINAL_STAGE = 3;
    public static final String RUN_MISMATCH = "run-mismatch", RUN_NOT_ACTIVE = "run-not-active",
        PACKET_NOT_ALLOWED = "packet-not-allowed", INTEGER_OUT_OF_RANGE = "integer-out-of-range";
    public static final String NOT_READY = "not-ready",
            NOT_LEADER = "not-leader",
            INVALID_PARTY = "invalid-party",
            JOIN_REJECTED = "join-rejected";
}
