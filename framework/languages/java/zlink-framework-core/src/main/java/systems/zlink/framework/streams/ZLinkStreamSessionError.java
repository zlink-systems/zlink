package systems.zlink.framework.streams;

public enum ZLinkStreamSessionError {
    INTERNAL(0),
    TRANSPORT_ERROR(1),
    HANDSHAKE_FAILED(2);

    private final int value;

    ZLinkStreamSessionError(int value) { this.value = value; }

    public int value() { return value; }
}
