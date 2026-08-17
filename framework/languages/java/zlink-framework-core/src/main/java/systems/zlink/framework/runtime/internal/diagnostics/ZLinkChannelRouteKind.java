package systems.zlink.framework.runtime.internal.diagnostics;

public enum ZLinkChannelRouteKind {
    ROUTE_MESH("route_mesh"),
    CLIENT_SERVER("client_server");

    private final String traceName;

    ZLinkChannelRouteKind(String traceName) {
        this.traceName = traceName;
    }

    public String traceName() {
        return traceName;
    }
}
