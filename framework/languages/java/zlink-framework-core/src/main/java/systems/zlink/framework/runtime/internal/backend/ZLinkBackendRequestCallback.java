package systems.zlink.framework.runtime.internal.backend;

@FunctionalInterface
public interface ZLinkBackendRequestCallback {
    void handle(ZLinkBackendReceived reply);
}
