package systems.zlink.stream.connector;

public enum ZLinkStreamCloseReason {
    CLIENT_CLOSE,
    IDLE_TIMEOUT,
    HEARTBEAT_TIMEOUT,
    SERVER_DRAIN,
    PROTOCOL_ERROR,
    TRANSPORT_ERROR
}
