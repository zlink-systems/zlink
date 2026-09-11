package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;

record ZLinkJavaSocketMonitor(SocketMonitor monitor) implements ZLinkBackendSocketMonitor {
    @Override public String name() { return "socketMonitor"; }
    @Override public ZLinkBackendSocketMonitorEvent recv() {
        while (!Thread.currentThread().isInterrupted()) {
            try {
                return fromMonitorEvent(monitor.recv());
            } catch (ZlinkRecvException timeout) {
                // A monitor uses the socket's bounded receive timeout. Its
                // empty queue is not a terminal monitor state: continue
                // waiting for the next lifecycle event.
                if (timeout.getResult() != RecvResult.NO_DATA) throw timeout;
            }
        }
        return null;
    }
    @Override public void close() { monitor.close(); }

    private static ZLinkBackendSocketMonitorEvent fromMonitorEvent(MonitorEvent event) {
        return new ZLinkBackendSocketMonitorEvent(
            event.event().name(),
            event.routingId(),
            event.localAddr(),
            event.remoteAddr());
    }
}
