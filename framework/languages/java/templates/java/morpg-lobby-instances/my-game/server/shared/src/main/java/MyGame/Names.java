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
    public static final List<String> SCENARIOS =
            java.util.Arrays.stream(Scenario.values()).map(Scenario::id).toList();
}
