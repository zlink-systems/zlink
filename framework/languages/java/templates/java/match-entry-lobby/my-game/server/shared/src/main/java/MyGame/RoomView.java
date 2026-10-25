package MyGame;

public record RoomView(
        String roomId,
        String nodeId,
        String phase,
        String winner,
        java.util.List<PlayerView> players) {}
