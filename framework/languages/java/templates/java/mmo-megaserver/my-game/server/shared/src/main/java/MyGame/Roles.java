package MyGame;

import java.util.List;

public final class Roles {
    private Roles() {}

    public static final String SESSION = "session",
            REGION = "region",
            INSTANCE = "instance",
            EXTERNAL = "external",
            LOCATION = "location-store";
    public static final List<String> STARTUP_ORDER = List.of(REGION, INSTANCE, SESSION);
}
