package systems.zlink.framework.runtime.internal.diagnostics;

public enum ZLinkMessageFlowResult {
    SUCCEEDED("succeeded"),
    FAILED("failed"),
    BACKPRESSURED("backpressured"),
    DROPPED("dropped"),
    CANCELLED("cancelled"),
    SHUTDOWN("shutdown");

    private final String traceName;

    ZLinkMessageFlowResult(String traceName) {
        this.traceName = traceName;
    }

    public String traceName() {
        return traceName;
    }
}
