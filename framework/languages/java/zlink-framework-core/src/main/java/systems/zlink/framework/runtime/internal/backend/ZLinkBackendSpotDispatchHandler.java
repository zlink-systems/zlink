package systems.zlink.framework.runtime.internal.backend;

@FunctionalInterface
public interface ZLinkBackendSpotDispatchHandler {
    void handle(ZLinkBackendSpotDispatchInfo info);
}
