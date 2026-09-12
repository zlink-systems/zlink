package systems.zlink.framework.errors;

public enum ZLinkFrameworkErrorKind {
    NOT_FOUND(0),
    ALREADY_EXISTS(1),
    TYPE_MISMATCH(2),
    NOT_CONFIGURED(3),
    REJECTED(4),
    UNAVAILABLE(5),
    DEADLINE_EXCEEDED(6),
    SHUTTING_DOWN(7),
    PROTOCOL_ERROR(8),
    INVALID_OPERATION(9),
    DATA_LOST(10),
    INTERNAL_FAILURE(11);

    private final int value;

    ZLinkFrameworkErrorKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static ZLinkFrameworkErrorKind fromValue(int value) {
        for (ZLinkFrameworkErrorKind kind : values()) {
            if (kind.value == value) {
                return kind;
            }
        }
        throw new IllegalArgumentException("unknown framework error kind: " + value);
    }
}
