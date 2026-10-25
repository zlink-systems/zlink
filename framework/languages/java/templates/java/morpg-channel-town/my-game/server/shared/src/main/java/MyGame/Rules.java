package MyGame;

public final class Rules {
    private Rules() {}

    public static final int MAX_MEMBERS = 4;
    public static final String JOIN_REJECTED = "join-rejected",
            INVALID_PARTY = "invalid-party",
            NOT_LEADER = "not-leader",
            RUN_MISMATCH = "run-mismatch",
            RUN_IN_PROGRESS = "run-in-progress",
            RUN_NOT_ACTIVE = "run-not-active",
            PACKET_NOT_ALLOWED = "packet-not-allowed",
            INTEGER_OUT_OF_RANGE = "integer-out-of-range",
            INVALID_CHANNEL = "invalid-channel";
}
