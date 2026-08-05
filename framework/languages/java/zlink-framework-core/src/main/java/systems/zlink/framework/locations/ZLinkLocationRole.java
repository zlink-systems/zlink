package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

public enum ZLinkLocationRole {
    INVALID(0),
    // Value 1 is reserved for the removed gateway role.
    SPOT(2),
    ROUTER(3),
    DEALER(4),
    PUB(5),
    SUB(6);

    private final int value;

    ZLinkLocationRole(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static ZLinkLocationRole fromValue(int value) {
        return switch (value) {
            case 2 -> SPOT;
            case 3 -> ROUTER;
            case 4 -> DEALER;
            case 5 -> PUB;
            case 6 -> SUB;
            default -> INVALID;
        };
    }
}
