package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorHandler;

record ZLinkJavaSocketMonitor(SocketMonitor monitor) implements ZLinkBackendSocketMonitor {
    @Override public String name() { return "socketMonitor"; }
    @Override public void onEvent(ZLinkBackendSocketMonitorHandler handler) {
        monitor.onEvent(event -> handler.handle(fromMonitorEvent(event)));
    }
    @Override public ZLinkBackendSocketMonitorEvent recv() { return fromMonitorEvent(monitor.recv()); }
    @Override public void close() { monitor.close(); }

    private static ZLinkBackendSocketMonitorEvent fromMonitorEvent(MonitorEvent event) {
        return new ZLinkBackendSocketMonitorEvent(
            event.event().name(),
            event.routingId(),
            event.localAddr(),
            event.remoteAddr());
    }
}
