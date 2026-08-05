package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

public enum ZLinkMeshNodeObjectRole {
    NONE(0),
    CLIENT(1),
    SERVER(2);

    private final int value;

    ZLinkMeshNodeObjectRole(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
