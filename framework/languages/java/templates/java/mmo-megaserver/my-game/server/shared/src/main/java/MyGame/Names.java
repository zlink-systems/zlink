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
    public static final List<String> SCENARIOS =
            java.util.Arrays.stream(Scenario.values()).map(Scenario::id).toList();

    public static String regionId(String region, int index) {
        if (region == null || region.isBlank() || index < 1)
            throw new IllegalArgumentException("Invalid region identity");
        return region + "-" + index;
    }
}
