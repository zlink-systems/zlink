package MyGame;

import org.jspecify.annotations.Nullable;

import java.util.List;

public record RoomView(
        String roomId,
        String nodeId,
        List<PlayerView> players,
        MatchPhase phase,
        @Nullable String winner) {}
