package MyGame;

public record PlayerMoved(String playerId, Position position, long sequence, String sourceSpotId) {
    public PlayerMoved(String playerId, Position position, long sequence) {
        this(playerId, position, sequence, "");
    }
}
