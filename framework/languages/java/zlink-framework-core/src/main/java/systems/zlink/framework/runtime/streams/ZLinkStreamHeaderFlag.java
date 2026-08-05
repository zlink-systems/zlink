package systems.zlink.framework.runtime.streams;

public enum ZLinkStreamHeaderFlag {
    HAS_REQUEST_SEQUENCE(0x01),
    HAS_METADATA(0x02),
    PAYLOAD_COMPRESSED(0x04),
    HAS_CORRELATION_ID(0x08),
    HAS_FLOW_ID(0x10);

    private final int value;

    ZLinkStreamHeaderFlag(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
