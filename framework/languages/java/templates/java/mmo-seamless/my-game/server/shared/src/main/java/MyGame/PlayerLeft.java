package MyGame;

public record PlayerLeft(String playerId, PlayerLeftReason reason, String sourceSpotId) {
    public PlayerLeft(String playerId, PlayerLeftReason reason) {
        this(playerId, reason, "");
    }
}
