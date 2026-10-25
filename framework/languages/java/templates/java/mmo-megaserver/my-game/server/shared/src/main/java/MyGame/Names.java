package MyGame;

import java.util.List;

public final class Names {
    private Names() {}

    public static final String LANGUAGE = "java",
            MESH = "my-game-world",
            STREAM = "players",
            PLAYER = "Player",
            REGION = "Region",
            DUNGEON = "Dungeon",
            REGION_FULL = "region-full";
    public static final long MAX_SAFE_INTEGER = 9_007_199_254_740_991L;
    public static final int MAX_REPLICAS_PER_REGION = 2;
    public static final int MAX_STEP = 10, REGION_MIN_X = 0, REGION_MAX_X = 100;
    public static final double MAX_SPEED = MAX_STEP * 5.0;
    public static final List<String> ALLOWED_REGIONS = List.of("forest", "desert");
    public static final List<String> SCENARIOS =
            java.util.Arrays.stream(Scenario.values()).map(Scenario::id).toList();

    public static String regionId(String region, int index) {
        if (region == null || region.isBlank() || index < 1)
            throw new IllegalArgumentException("Invalid region identity");
        return region + "-" + index;
    }
}
