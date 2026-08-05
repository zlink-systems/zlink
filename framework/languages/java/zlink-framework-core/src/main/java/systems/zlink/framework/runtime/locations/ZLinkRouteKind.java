package systems.zlink.framework.runtime.locations;

enum ZLinkRouteKind {
    INVALID(0),
    ACTOR_SESSION(1),
    SPOT_NAME(2),
    FRAMEWORK_ROUTE(3);

    private final int value;

    ZLinkRouteKind(int value) {
        this.value = value;
    }

    int value() {
        return value;
    }
}
