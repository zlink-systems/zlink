package MyGame;

public record QueueRequest(String playerId, String mode) {
    public QueueRequest {
        if (mode == null) mode = Names.DEFAULT_MODE;
    }

    public QueueRequest(String playerId) {
        this(playerId, Names.DEFAULT_MODE);
    }
}
