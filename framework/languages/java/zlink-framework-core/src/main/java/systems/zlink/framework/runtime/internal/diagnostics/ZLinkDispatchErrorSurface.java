package systems.zlink.framework.runtime.internal.diagnostics;

public enum ZLinkDispatchErrorSurface {
    CHANNEL(0, "channel"),
    ROUTE_MESH_CHANNEL(1, "channel"),
    SPOT_ROUTE(2, "spot"),
    SPOT_SUBSCRIPTION(3, "spot"),
    SPOT_ACTOR(4, "actor"),
    STREAM_SESSION(5, "stream"),
    NODE(6, "node"),
    INSTANCE_SPOT(7, "instance_spot"),
    ACTOR_RELOCATION(8, "actor_relocation"),
    CLASSIC_FANOUT(9, "classic_fanout");

    private final int value;
    private final String traceName;

    ZLinkDispatchErrorSurface(int value, String traceName) {
        this.value = value;
        this.traceName = traceName;
    }

    public int value() { return value; }

    public String traceName() { return traceName; }
}
