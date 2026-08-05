package systems.zlink.framework.streams;

public enum ZLinkStreamMessageKind {
    SEND(1),
    REQUEST(2),
    RESPONSE(3),
    ERROR(4),
    CONTROL(5);

    private final int value;

    ZLinkStreamMessageKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static ZLinkStreamMessageKind fromValue(int value) {
        for (ZLinkStreamMessageKind kind : values()) {
            if (kind.value == value) {
                return kind;
            }
        }
        throw new IllegalArgumentException("unknown stream message kind: " + value);
    }
}
