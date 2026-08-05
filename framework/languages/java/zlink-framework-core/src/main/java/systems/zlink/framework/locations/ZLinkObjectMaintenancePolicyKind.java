package systems.zlink.framework.locations;

public enum ZLinkObjectMaintenancePolicyKind {
    DISABLED(1),
    RECREATE(2),
    SNAPSHOT(3);

    private final int value;

    ZLinkObjectMaintenancePolicyKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
