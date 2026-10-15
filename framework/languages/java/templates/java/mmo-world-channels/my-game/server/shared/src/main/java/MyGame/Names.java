package MyGame;

import java.util.List;

public final class Names {
    private Names() {}

    public static final String LANGUAGE = "java",
            WORLD_A = "A",
            WORLD_B = "B",
            WORLD_LIST_PATH = "/worlds",
            STREAM = "players",
            PLAYER = "Player",
            ZONE = "Zone",
            PLAYER_KEY = "player:",
            FIELD = "field@";
    public static final int FIRST_CHANNEL = 1;
    public static final long MAX_GOLD = 9_007_199_254_740_991L;
    public static final List<String> SCENARIOS =
            java.util.Arrays.stream(Scenario.values()).map(Scenario::id).toList();

    public static String mesh(String world) {
        return switch (world) {
            case WORLD_A -> "my-game-world-a";
            case WORLD_B -> "my-game-world-b";
            default -> throw new IllegalArgumentException("Unknown world: " + world);
        };
    }

    public static String zone(int channel) {
        if (channel < 1) throw new IllegalArgumentException("Channel must be positive");
        return FIELD + channel;
    }

    public static int channel(String zone) {
        if (!zone.startsWith(FIELD)) throw new IllegalArgumentException("Invalid Zone ID");
        int n = Integer.parseInt(zone.substring(FIELD.length()));
        if (!zone(n).equals(zone)) throw new IllegalArgumentException("Invalid channel");
        return n;
    }
}
