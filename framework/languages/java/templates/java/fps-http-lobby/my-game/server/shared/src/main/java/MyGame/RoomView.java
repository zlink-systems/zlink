package MyGame;

import java.util.List;
import org.jspecify.annotations.Nullable;

public record RoomView(
        String roomId, String nodeId, List<PlayerView> players, MatchPhase phase, @Nullable String winner) {}
