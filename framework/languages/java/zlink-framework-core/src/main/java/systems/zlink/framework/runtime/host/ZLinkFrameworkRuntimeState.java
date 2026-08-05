package systems.zlink.framework.runtime.host;

public enum ZLinkFrameworkRuntimeState {
    PREPARING(0),
    SERVING(1),
    RELOCATING(2),
    RELOCATED(3),
    DRAINING(4),
    STOPPED(5),
    ERROR(6);

    private final int wireValue;

    ZLinkFrameworkRuntimeState(int wireValue) {
        this.wireValue = wireValue;
    }

    public int wireValue() {
        return wireValue;
    }
}
