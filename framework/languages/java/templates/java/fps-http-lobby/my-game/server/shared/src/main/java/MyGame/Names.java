package MyGame;

import java.util.List;

public final class Names {
    private Names() {}

    public static final String LANGUAGE = "java",
            MESH = "my-game-arena",
            STREAM = "players",
            PLAYER = "Player",
            ROOM = "Room",
            MATCH_PATH = "/match",
            ROOM_PREFIX = "room-",
            DEFAULT_MODE = "duel";
    public static final List<String> SCENARIOS =
            java.util.Arrays.stream(Scenario.values()).map(Scenario::id).toList();
}
