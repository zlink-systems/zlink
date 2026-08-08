package systems.zlink.framework.runtime.internal.diagnostics;

public enum ZLinkDispatchErrorSurface {
    CHANNEL(0),
    ROUTE_MESH_CHANNEL(1),
    SPOT_ROUTE(2),
    SPOT_SUBSCRIPTION(3),
    SPOT_ACTOR(4),
    STREAM_SESSION(5);

    private final int value;

    ZLinkDispatchErrorSurface(int value) { this.value = value; }

    public int value() { return value; }
}
