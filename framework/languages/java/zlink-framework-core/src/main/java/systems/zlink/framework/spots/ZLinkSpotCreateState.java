package systems.zlink.framework.spots;

public enum ZLinkSpotCreateState {
    EXISTING(0),
    CREATED(1),
    REJECTED(2);

    private final int value;

    ZLinkSpotCreateState(int value) { this.value = value; }

    public int value() { return value; }
}
