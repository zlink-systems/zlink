package systems.zlink.framework.runtime.internal.backend;

@FunctionalInterface
public interface ZLinkBackendSocketMonitorHandler {
    void handle(ZLinkBackendSocketMonitorEvent event);
}
