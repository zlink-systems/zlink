package systems.zlink.framework.streams;

public enum ZLinkDisconnectReason {
    UNKNOWN(0),
    HANDSHAKE_FAILED(3),
    TRANSPORT_ERROR(4),
    CONTEXT_TERMINATED(5);

    private final int value;

    ZLinkDisconnectReason(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
