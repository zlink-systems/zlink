package MyGame;

import java.util.Arrays;
import java.util.List;

public final class Names {
    private Names() {}

    public static final String MESH = "my-game-arena",
            STREAM = "players",
            PLAYER = "Player",
            ROOM = "Room",
            MATCHMAKING = "Matchmaking",
            MATCHMAKING_ID = "matchmaking",
            DEFAULT_MODE = "duel",
            ROOM_PREFIX = "room-",
            LANGUAGE = "java";
    public static final int MATCHMAKING_CAPACITY = 1;
    public static final List<String> SCENARIOS =
            Arrays.stream(Scenario.values()).map(Scenario::id).toList();
}
