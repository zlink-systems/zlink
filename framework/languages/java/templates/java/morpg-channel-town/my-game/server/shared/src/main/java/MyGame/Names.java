package MyGame;

import java.util.List;

public final class Names {
    private Names() {}

    public static final String LANGUAGE = "java",
            MESH = "my-game-world",
            STREAM = "players",
            PLAYER = "Player",
            TOWN = "Town",
            DUNGEON = "Dungeon",
            TOWN_PREFIX = "town-",
            DUNGEON_PREFIX = "dungeon-";
    public static final List<String> SCENARIOS =
            java.util.Arrays.stream(Scenario.values()).map(Scenario::id).toList();
    public static final int CHANNEL_COUNT = 2;

    public static String townId(int channel) {
        if (channel < 1 || channel > CHANNEL_COUNT)
            throw new IllegalArgumentException("Invalid channel");
        return TOWN_PREFIX + channel;
    }
}
