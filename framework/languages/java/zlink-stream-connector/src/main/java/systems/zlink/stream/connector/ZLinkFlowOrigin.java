package systems.zlink.stream.connector;

/**
 * Identifies where a stream flow was first created.
 */
public enum ZLinkFlowOrigin {
    INBOUND(1),
    TIMER(2),
    APPLICATION(3),
    LIFECYCLE(4);

    private final int wireValue;

    ZLinkFlowOrigin(int wireValue) {
        this.wireValue = wireValue;
    }

    static ZLinkFlowOrigin fromWireValue(int wireValue) {
        for (ZLinkFlowOrigin origin : values()) {
            if (origin.wireValue == wireValue) {
                return origin;
            }
        }
        throw new IllegalArgumentException("unknown stream flow origin: " + wireValue);
    }
}
