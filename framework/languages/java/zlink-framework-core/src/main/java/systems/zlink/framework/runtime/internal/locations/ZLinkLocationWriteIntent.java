package systems.zlink.framework.runtime.internal.locations;

public enum ZLinkLocationWriteIntent {
    NEW_CLAIM(1),
    RENEW(2),
    TAKEOVER(3);

    private final int value;

    ZLinkLocationWriteIntent(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
