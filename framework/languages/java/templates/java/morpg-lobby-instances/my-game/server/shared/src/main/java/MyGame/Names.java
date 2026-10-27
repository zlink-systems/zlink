package MyGame;

import java.util.List;

public final class Names {
    private Names() {}

    public static final String LANGUAGE = "java",
            MESH = "my-game-world",
            STREAM = "players",
            PLAYER = "Player",
            PARTY = "Party",
            DUNGEON = "Dungeon",
            PARTY_PREFIX = "party-",
            DUNGEON_PREFIX = "dungeon-";
    public static String newPartyId() { return PARTY_PREFIX + UUID(); }
    public static String newRunId() { return "run-" + UUID(); }
    private static String UUID() { return java.util.UUID.randomUUID().toString().replace("-", ""); }
    public static String dungeonId(String runId) { return DUNGEON_PREFIX + runId; }
    public static final List<String> SCENARIOS =
            java.util.Arrays.stream(Scenario.values()).map(Scenario::id).toList();
}
