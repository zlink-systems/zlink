package systems.zlink.framework.spots;

public enum ZLinkSpotKind {
    INVALID(0),
    ENTRY(1),
    USER(2),
    INSTANCE(3);

    private final int value;

    ZLinkSpotKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static ZLinkSpotKind fromValue(int value) {
        return switch (value) {
            case 1 -> ENTRY;
            case 2 -> USER;
            case 3 -> INSTANCE;
            default -> INVALID;
        };
    }
}
