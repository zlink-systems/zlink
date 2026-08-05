package systems.zlink.framework.runtime.internal.backend;

public interface ZLinkMonitoringBackendAdapter {
    ZLinkBackendSocketMonitor openSocketMonitor(ZLinkBackendSocket socket);
}
