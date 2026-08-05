package systems.zlink.framework.runtime.host;

enum ZLinkTerminationIntent {
    RETIRE(0),
    SHUTDOWN(1);

    private final int wireValue;

    ZLinkTerminationIntent(int wireValue) {
        this.wireValue = wireValue;
    }

    public int wireValue() {
        return wireValue;
    }
}
