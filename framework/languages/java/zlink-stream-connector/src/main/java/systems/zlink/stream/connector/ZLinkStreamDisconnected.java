package systems.zlink.stream.connector;

public record ZLinkStreamDisconnected(ZLinkStreamCloseReason closeReason) {
    public ZLinkStreamDisconnected {
        if (closeReason == null) {
            throw new IllegalArgumentException("closeReason is required");
        }
    }
}
