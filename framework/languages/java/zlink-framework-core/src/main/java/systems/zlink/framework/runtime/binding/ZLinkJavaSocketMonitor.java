package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaSocketMonitor implements ZLinkBackendSocketMonitor {
    private static final long MONITOR_SLOT = 1L;
    private static final long CLOSE_WAIT_DIAGNOSTIC_NANOS = TimeUnit.SECONDS.toNanos(1);
    private static final String WATCHDOG_DUMP_DIR = System.getenv("ZLINK_JAVA_WATCHDOG_DUMP_DIR");

    final SocketMonitor monitor;
    final Poller poller;
    private final PollEvents events = new PollEvents(1);
    private volatile boolean closed;
    boolean waiting;

    ZLinkJavaSocketMonitor(SocketMonitor monitor) {
        this(monitor, Zlink.createPoller());
        poller.add(monitor, MONITOR_SLOT, PollEventFlags.POLLIN);
    }

    ZLinkJavaSocketMonitor(SocketMonitor monitor, Poller poller) {
        this.monitor = Objects.requireNonNull(monitor, "monitor");
        this.poller = Objects.requireNonNull(poller, "poller");
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
                boolean closingSource = !closed;
                Throwable sourceFailure = null;
                try {
                    if (closingSource) {
                        closed = true;
                        monitor.close();
                    }
                } catch (RuntimeException | Error failure) {
                    sourceFailure = failure;
                    throw failure;
                } finally {
                    boolean diagnosticPending = closingSource && waiting;
                    long waitStarted = diagnosticPending ? System.nanoTime() : 0;
                    while (waiting) {
                        try {
                            if (diagnosticPending) {
                                long elapsed = System.nanoTime() - waitStarted;
                                long remaining = CLOSE_WAIT_DIAGNOSTIC_NANOS - elapsed;
                                if (remaining > 0) {
                                    TimeUnit.NANOSECONDS.timedWait(this, remaining);
                                    continue;
                                }
                                reportSlowClose(sourceFailure, elapsed);
                                diagnosticPending = false;
                            }
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

    private void reportSlowClose(Throwable sourceFailure, long elapsedNanos) {
        String result =
                switch (sourceFailure) {
                    case null -> "0";
                    case ZlinkCloseException failure -> Integer.toString(failure.getCode());
                    default -> "unavailable";
                };
        String line =
                "STREAM monitor close"
                        + " nativeCloseResult="
                        + result
                        + " waiting="
                        + waiting
                        + " elapsedMs="
                        + TimeUnit.NANOSECONDS.toMillis(elapsedNanos)
                        + " closeThreadId="
                        + Thread.currentThread().threadId();
        if (WATCHDOG_DUMP_DIR != null && !WATCHDOG_DUMP_DIR.isBlank()) {
            String fileName = "monitor-close-pid-" + ProcessHandle.current().pid() + ".log";
            try {
                Files.writeString(
                        Path.of(WATCHDOG_DUMP_DIR).resolve(fileName),
                        line + System.lineSeparator(),
                        StandardOpenOption.CREATE,
                        StandardOpenOption.APPEND);
                return;
            } catch (IOException | InvalidPathException | SecurityException failure) {
                System.getLogger(getClass().getName())
                        .log(System.Logger.Level.WARNING, line, failure);
                return;
            }
        }
        System.getLogger(getClass().getName()).log(System.Logger.Level.WARNING, line);
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
