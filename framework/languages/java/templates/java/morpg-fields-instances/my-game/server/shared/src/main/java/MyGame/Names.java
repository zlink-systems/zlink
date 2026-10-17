package MyGame;

import java.util.List;

public final class Names {
    private Names() {}

    public static final String LANGUAGE = "java",
            MESH = "my-game-world",
            STREAM = "players",
            PLAYER = "Player",
            FIELD = "Field",
            DUNGEON = "Dungeon",
            BORDER = "border",
            BORDER_TIMER = "border-tick";
    public static final List<String> SCENARIOS =
            java.util.Arrays.stream(Scenario.values()).map(Scenario::id).toList();
}
