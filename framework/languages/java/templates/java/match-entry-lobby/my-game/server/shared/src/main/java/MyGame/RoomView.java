package MyGame;

import org.jspecify.annotations.Nullable;

public record RoomView(
        String roomId,
        String nodeId,
        String phase,
        @Nullable String winner,
        java.util.List<PlayerView> players) {}
