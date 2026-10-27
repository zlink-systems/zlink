package MyGame;

import java.util.List;

public record RoomView(
        String roomId, String nodeId, List<PlayerView> players, MatchPhase phase, String winner) {}
