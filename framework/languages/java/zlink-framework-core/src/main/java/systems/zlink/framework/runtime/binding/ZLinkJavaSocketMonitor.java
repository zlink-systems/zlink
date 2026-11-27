package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.ZlinkTimer;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;

import java.time.Duration;
import java.util.Objects;

final class ZLinkJavaSocketMonitor implements ZLinkBackendSocketMonitor {
    private static final long MONITOR_SLOT = 1L;
    private static final long WAKE_SLOT = 2L;

    final SocketMonitor monitor;
    final Poller poller;
    private final ZlinkTimer wakeTimer;
    private final PollEvents events = new PollEvents(2);
    private volatile boolean closed;
    boolean waiting;

    ZLinkJavaSocketMonitor(SocketMonitor monitor) {
        this(monitor, Zlink.createPoller());
    }

    ZLinkJavaSocketMonitor(SocketMonitor monitor, Poller poller) {
        this.monitor = Objects.requireNonNull(monitor, "monitor");
        this.poller = Objects.requireNonNull(poller, "poller");
        ZlinkTimer timer = null;
        try {
            timer = Zlink.createTimer();
            poller.add(monitor, MONITOR_SLOT, PollEventFlags.POLLIN);
            poller.add(timer, WAKE_SLOT);
        } catch (RuntimeException | Error failure) {
            try (var cleanup = timer;
                    monitor;
                    poller) {
                throw failure;
            }
        }
        this.wakeTimer = timer;
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
        boolean readable;
        try {
            readable = poller.wait(events, timeout) > 0;
        } finally {
            synchronized (this) {
                waiting = false;
                notifyAll();
            }
        }
        return readable && !closed;
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
                if (closed) {
                    return;
                }
                if (waiting) {
                    wakeTimer.start(Duration.ofNanos(1), 1);
                }
                closed = true;
                // The poller retains both native sources until its active wait returns.
                while (waiting) {
                    try {
                        wait();
                    } catch (InterruptedException interruption) {
                        interrupted = true;
                    }
                }
                try (wakeTimer;
                        poller) {
                    monitor.close();
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
                event.flags(),
                event.value());
    }
}
