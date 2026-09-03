package systems.zlink.framework.runtime.internal.backend;

public interface ZLinkBackendSocketMonitor extends ZLinkBackendObject {
    ZLinkBackendSocketMonitorEvent recv();
}
