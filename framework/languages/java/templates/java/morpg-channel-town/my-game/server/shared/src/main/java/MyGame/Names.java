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
    public static final long CHANNEL_COUNT = 2;
    public static final long MAXIMUM_INTEGER = 9_007_199_254_740_991L;
    public static final long FINAL_STAGE = 3;

    public static String townId(long channel) {
        if (channel < 1 || channel > CHANNEL_COUNT)
            throw new IllegalArgumentException("Invalid channel");
        return TOWN_PREFIX + channel;
    }

    public static String dungeonId(String runId) {
        return DUNGEON_PREFIX + runId;
    }

    public static boolean validId(String value, String prefix) {
        if (value == null || !value.startsWith(prefix) || value.length() != prefix.length() + 32)
            return false;
        for (int i = prefix.length(); i < value.length(); i++) {
            char c = value.charAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }
}
