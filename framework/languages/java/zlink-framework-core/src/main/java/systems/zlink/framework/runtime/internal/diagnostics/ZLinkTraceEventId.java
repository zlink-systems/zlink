package systems.zlink.framework.runtime.internal.diagnostics;

public enum ZLinkTraceEventId {
    MESSAGE_FLOW("zlink.message_flow"),
    DISPATCH_ERROR("zlink.dispatch_error");

    private final String traceName;

    ZLinkTraceEventId(String traceName) {
        this.traceName = traceName;
    }

    public String traceName() {
        return traceName;
    }
}
