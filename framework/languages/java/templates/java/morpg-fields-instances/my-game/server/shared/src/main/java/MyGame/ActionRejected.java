package MyGame;

public record ActionRejected(String reason, int retryAfterMs) {
    public ActionRejected(String reason) {
        this(reason, 0);
    }
}
