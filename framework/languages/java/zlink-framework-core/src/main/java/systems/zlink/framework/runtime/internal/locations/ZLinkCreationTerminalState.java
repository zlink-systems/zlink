package systems.zlink.framework.runtime.internal.locations;

public enum ZLinkCreationTerminalState {
    CREATED(1),
    REJECTED(2),
    FAILED(3);

    private final int value;

    ZLinkCreationTerminalState(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
