package MyGame;

import java.util.List;

public final class Names {
    private Names() {}

    public static final String LANGUAGE = "java",
            MESH = "my-game-tables",
            API = "player-api",
            STREAM = "players",
            PLAYER = "Player",
            TABLE = "Table",
            POOL = "Pool",
            POOL_KEY = "pool-board";
    public static final List<String> SCENARIOS =
            java.util.Arrays.stream(Scenario.values()).map(Scenario::id).toList();
}
