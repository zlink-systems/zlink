package systems.zlink.framework.errors;

public enum ZLinkFrameworkErrorKind {
    NOT_FOUND(0),
    ALREADY_EXISTS(1),
    TYPE_MISMATCH(2),
    NOT_CONFIGURED(3),
    REJECTED(4),
    UNAVAILABLE(5),
    CAPACITY_EXCEEDED(6),
    DEADLINE_EXCEEDED(7),
    SHUTTING_DOWN(8),
    PROTOCOL_ERROR(9),
    INVALID_OPERATION(10),
    DATA_LOST(11),
    INTERNAL_FAILURE(12);

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
