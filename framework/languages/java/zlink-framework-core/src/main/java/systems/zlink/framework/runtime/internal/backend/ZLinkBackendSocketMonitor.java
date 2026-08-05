package systems.zlink.framework.runtime.internal.backend;

public interface ZLinkBackendSocketMonitor extends ZLinkBackendObject {
    void onEvent(ZLinkBackendSocketMonitorHandler handler);

    ZLinkBackendSocketMonitorEvent recv();
}
