package systems.zlink.framework.locations;

public enum ZLinkPlacementObjectKind {
    ACTOR(1),
    USER_SPOT(2),
    INSTANCE_SPOT(3);

    private final int value;

    ZLinkPlacementObjectKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
