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
    private volatile boolean closed;
    private boolean waiting;

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
    public boolean waitForReadable(Duration timeout) {
        synchronized (this) {
            if (closed) {
                return false;
            }
            waiting = true;
        }
        try {
            return poller.wait(events, timeout) > 0 && !closed;
        } finally {
            synchronized (this) {
                waiting = false;
                notifyAll();
            }
        }
    }

    @Override
    public synchronized ZLinkBackendSocketMonitorEvent recvDontWait() {
        if (closed) {
            return null;
        }
        MonitorEvent event = monitor.recv(RecvFlags.DONT_WAIT);
        return event == null ? null : fromMonitorEvent(event);
    }

    @Override
    public boolean isClosed() {
        return closed;
    }

    @Override
    public void close() {
        boolean interrupted = false;
        try {
            synchronized (this) {
                Throwable sourceFailure = null;
                try {
                    if (!closed) {
                        closed = true;
                        monitor.close();
                    }
                } catch (RuntimeException | Error failure) {
                    sourceFailure = failure;
                    throw failure;
                } finally {
                    while (waiting) {
                        try {
                            wait();
                        } catch (InterruptedException interruption) {
                            interrupted = true;
                        }
                    }
                    try {
                        poller.close();
                    } catch (RuntimeException | Error failure) {
                        if (sourceFailure == null) {
                            throw failure;
                        }
                        sourceFailure.addSuppressed(failure);
                    }
                }
            }
        } finally {
            if (interrupted) {
                Thread.currentThread().interrupt();
            }
        }
    }

    private static ZLinkBackendSocketMonitorEvent fromMonitorEvent(MonitorEvent event) {
        return new ZLinkBackendSocketMonitorEvent(
                event.event().name(),
                event.routingId(),
                event.localAddr(),
                event.remoteAddr(),
                event.flags());
    }
}
