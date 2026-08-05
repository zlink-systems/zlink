package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

public enum ZLinkLocationTopologyState {
    DISCOVERED(1),
    CONNECTING(2),
    READY(3),
    LOST(4),
    ERROR(5),
    STOPPED(6);

    private final int value;

    ZLinkLocationTopologyState(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
