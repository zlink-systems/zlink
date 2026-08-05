package systems.zlink.framework.runtime.host;

public enum ZLinkFrameworkRelocationMode {
    PLANNED_MAINTENANCE(0),
    ROLLING_UPDATE(1);

    private final int wireValue;

    ZLinkFrameworkRelocationMode(int wireValue) {
        this.wireValue = wireValue;
    }

    public int wireValue() {
        return wireValue;
    }
}
