package MyGame;

public record PlayerAppeared(String playerId, Position position, String sourceSpotId) {
    public PlayerAppeared(String playerId, Position position) {
        this(playerId, position, "");
    }
}
