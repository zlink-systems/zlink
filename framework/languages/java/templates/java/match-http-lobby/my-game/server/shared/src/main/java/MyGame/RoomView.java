package MyGame;

import org.jspecify.annotations.Nullable;

public record RoomView(
        String roomId,
        String nodeId,
        java.util.List<PlayerView> players,
        String phase,
        @Nullable String winner) {}
