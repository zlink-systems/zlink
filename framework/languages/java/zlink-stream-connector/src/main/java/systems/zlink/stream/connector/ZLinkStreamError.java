package systems.zlink.stream.connector;

public record ZLinkStreamError(
    ZLinkStreamErrorCode code,
    String message,
    Throwable exception) {
    public ZLinkStreamError {
        if (code == null) {
            throw new IllegalArgumentException("code is required");
        }
        if (message == null || message.isBlank()) {
            throw new IllegalArgumentException("message is required");
        }
    }

    public ZLinkStreamError(
        ZLinkStreamErrorCode code,
        String message) {
        this(code, message, null);
    }
}
