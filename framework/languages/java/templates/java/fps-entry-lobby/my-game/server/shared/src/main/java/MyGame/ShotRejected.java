package MyGame;

public record ShotRejected(String reason, int retryAfterMs) {
    public ShotRejected(String reason) {
        this(reason, 0);
    }
}
