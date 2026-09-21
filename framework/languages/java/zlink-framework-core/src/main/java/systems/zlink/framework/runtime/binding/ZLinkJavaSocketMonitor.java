package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;

import java.time.Duration;
import java.util.Objects;

final class ZLinkJavaSocketMonitor implements ZLinkBackendSocketMonitor {
    private static final long MONITOR_SLOT = 1L;

    private final SocketMonitor monitor;
    private final Poller poller;
    private final PollEvents events = new PollEvents(1);
    // The drain loop holds the monitor for the length of a poll wait, so a close
    // that needed the same lock could be starved by the loop reacquiring it.
    // Requesting the close is therefore a volatile write the loop reads without
    // locking; only the native close itself takes the monitor.
    private volatile boolean closeRequested;
    private boolean closed;

    ZLinkJavaSocketMonitor(SocketMonitor monitor) {
        this.monitor = Objects.requireNonNull(monitor, "monitor");
        poller = Zlink.createPoller();
        poller.add(monitor, MONITOR_SLOT, PollEventFlags.POLLIN);
    }

    @Override
    public String name() {
        return "socketMonitor";
    }

    @Override
    public synchronized boolean waitForReadable(Duration timeout) {
        if (closed || closeRequested) {
            return false;
        }
        return poller.wait(events, timeout) > 0;
    }

    @Override
    public synchronized ZLinkBackendSocketMonitorEvent recvDontWait() {
        if (closed || closeRequested) {
            return null;
        }
        MonitorEvent event = monitor.recv(RecvFlags.DONT_WAIT);
        return event == null ? null : fromMonitorEvent(event);
    }

    @Override
    public boolean isClosed() {
        return closeRequested;
    }

    @Override
    public void close() {
        closeRequested = true;
        synchronized (this) {
            if (closed) {
                return;
            }
            closed = true;
            try {
                poller.close();
            } finally {
                monitor.close();
            }
        }
    }

    private static ZLinkBackendSocketMonitorEvent fromMonitorEvent(MonitorEvent event) {
        return new ZLinkBackendSocketMonitorEvent(
                event.event().name(), event.routingId(), event.localAddr(), event.remoteAddr());
    }
}
