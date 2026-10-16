package MyGame;

import java.time.Duration;

public final class Control {
    private Control() {}

    public static final String DRAIN = "drain",
            SHUTDOWN = "shutdown",
            READY = "READY ",
            RELOCATION_REQUESTED = "RELOCATION_REQUESTED",
            RELOCATED = "RELOCATED",
            SAFE_TO_SHUTDOWN = "SAFE_TO_SHUTDOWN",
            STOPPED = "STOPPED",
            PROCESS_EXIT = "PROCESS_EXIT ";
    public static final Duration HOST_DEADLINE = Duration.ofSeconds(30);
    public static final Duration MESSAGE_FOLLOW = Duration.ofSeconds(30);
    public static final Duration SAFE_DEADLINE = MESSAGE_FOLLOW.plus(HOST_DEADLINE);
    public static final long PROCESS_SECONDS = SAFE_DEADLINE.plusSeconds(5).toSeconds();
}
