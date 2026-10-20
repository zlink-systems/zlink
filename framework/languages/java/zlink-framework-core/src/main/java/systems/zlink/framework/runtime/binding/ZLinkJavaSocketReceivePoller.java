package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.PollSourceKind;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.ZlinkTimer;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;

import java.time.Duration;
import java.util.Objects;
import java.util.function.Supplier;

/**
 * Owns the public zlink Poller used to guard one Framework socket receive.
 *
 * <p>The Framework asks this object whether the socket is readable before it calls the socket's
 * public receive method. The poller and its reusable event storage remain binding-owned, so receive
 * loops do not know binding details.
 */
final class ZLinkJavaSocketReceivePoller implements AutoCloseable {
    private static final long SOCKET_SLOT = 1L;
    private static final long WAKE_SLOT = 2L;
    private static final Duration WAKE_INTERVAL = Duration.ofNanos(1);
    private static final long WAKE_FIRE_COUNT = 1;
    static final int READABLE = 1;
    static final int ROUTE_CHANGED = 2;
    static final int OWNER_WAKE = 4;

    private final Socket socket;
    private final Poller poller;
    private final ZlinkTimer wakeTimer;
    private final boolean ownsCompletionQueue;
    private final boolean observesRoutes;
    private final PollEvents events;
    private volatile boolean registered;
    private volatile boolean closed;
    // Last physical timer registration; null means an explicit owner notification is pending.
    // The logical deadline remains owned by the management registry.
    private volatile Long scheduledManagementDeadline = ZLinkServiceLivenessRegistry.NO_DEADLINE;

    ZLinkJavaSocketReceivePoller(Socket socket) {
        this(socket, true, false);
    }

    ZLinkJavaSocketReceivePoller(Socket socket, boolean ownsCompletionQueue) {
        this(socket, ownsCompletionQueue, false);
    }

    /**
     * {@code observesRoutes} also waits for ROUTER {@code POLLROUTE}. That readiness stays set
     * until a routes snapshot succeeds (Core ROUTER §10.1), so only the socket's one route observer
     * may request it.
     */
    ZLinkJavaSocketReceivePoller(
            Socket socket, boolean ownsCompletionQueue, boolean observesRoutes) {
        this(
                socket,
                ownsCompletionQueue,
                observesRoutes,
                Zlink::createPoller,
                observesRoutes ? Zlink::createTimer : () -> null);
    }

    ZLinkJavaSocketReceivePoller(
            Socket socket,
            boolean ownsCompletionQueue,
            boolean observesRoutes,
            Supplier<Poller> pollerFactory,
            Supplier<ZlinkTimer> wakeTimerFactory) {
        Objects.requireNonNull(socket, "socket");
        this.socket = socket;
        this.ownsCompletionQueue = ownsCompletionQueue;
        this.observesRoutes = observesRoutes;
        this.poller = Objects.requireNonNull(pollerFactory.get(), "poller");
        ZlinkTimer timer = null;
        boolean accepted = false;
        try {
            timer = wakeTimerFactory.get();
            events = new PollEvents(timer == null ? 1 : 2);
            this.wakeTimer = timer;
            accepted = true;
        } finally {
            if (!accepted) {
                try {
                    if (timer != null) timer.close();
                } finally {
                    this.poller.close();
                }
            }
        }
    }

    void ensureRegistered() {
        if (registered) {
            return;
        }
        synchronized (this) {
            if (closed || registered) {
                return;
            }
            // Framework creates the wrapper before it applies routing options
            // and calls bind/connect. Register the fully configured socket when
            // the receive owner first needs readiness. For socket kinds that
            // support it, the binding owns async DONTWAIT retry and completion
            // draining on this public poller.
            if (observesRoutes) {
                poller.add(
                        socket,
                        SOCKET_SLOT,
                        PollEventFlags.POLLIN,
                        PollEventFlags.POLLOUT,
                        PollEventFlags.POLLCOMPLETION,
                        PollEventFlags.POLLROUTE);
            } else if (ownsCompletionQueue) {
                poller.add(
                        socket,
                        SOCKET_SLOT,
                        PollEventFlags.POLLIN,
                        PollEventFlags.POLLOUT,
                        PollEventFlags.POLLCOMPLETION);
            } else {
                poller.add(socket, SOCKET_SLOT, PollEventFlags.POLLIN);
            }
            if (wakeTimer != null) poller.add(wakeTimer, WAKE_SLOT);
            registered = true;
        }
    }

    synchronized boolean waitForReadable(Duration timeout) {
        return (waitForReady(timeout) & READABLE) != 0;
    }

    /** Waits once and returns the {@link #READABLE} and {@link #ROUTE_CHANGED} bits it reported. */
    synchronized int waitForReady(Duration timeout) {
        int ready = waitForOwnerReady(timeout);
        if ((ready & OWNER_WAKE) != 0) consumeOwnerWake();
        return ready & ~OWNER_WAKE;
    }

    synchronized int waitForOwnerReady(Duration timeout) {
        if (closed) {
            return 0;
        }
        ensureRegistered();
        int count = poller.wait(events, timeout);
        int ready = 0;
        for (int index = 0; index < count; index++) {
            if (events.sourceKind(index) == PollSourceKind.TIMER
                    && events.slot(index) == WAKE_SLOT) {
                ready |= OWNER_WAKE;
                continue;
            }
            if (events.sourceKind(index) != PollSourceKind.SOCKET
                    || events.slot(index) != SOCKET_SLOT) {
                continue;
            }
            if (isReadable(events, index)) {
                ready |= READABLE;
            }
            if (observesRoutes && events.hasEvent(index, PollEventFlags.POLLROUTE)) {
                ready |= ROUTE_CHANGED;
            }
        }
        return ready;
    }

    // The port's existing state lane and close claim serialize producers with disposal.
    // Do not acquire this monitor: the receive owner holds it during native wait.
    void signal() {
        if (!closed && wakeTimer != null) {
            wakeTimer.start(WAKE_INTERVAL, WAKE_FIRE_COUNT);
            scheduledManagementDeadline = null;
        }
    }

    // The port invokes preparation and consumption on its existing state lane, alongside signal.
    void prepareManagementWait(long deadline, long now) {
        ensureRegistered();
        if (scheduledManagementDeadline == null || scheduledManagementDeadline == deadline) return;
        if (deadline == ZLinkServiceLivenessRegistry.NO_DEADLINE) wakeTimer.stop();
        else
            wakeTimer.start(
                    Duration.ofNanos(Math.max(WAKE_INTERVAL.toNanos(), deadline - now)),
                    WAKE_FIRE_COUNT);
        scheduledManagementDeadline = deadline;
    }

    void consumeOwnerWake() {
        wakeTimer.recv();
        if (scheduledManagementDeadline == null)
            scheduledManagementDeadline = ZLinkServiceLivenessRegistry.NO_DEADLINE;
    }

    private static boolean isReadable(PollEvents events, int index) {
        // A socket close or exceptional receive condition is still a receive
        // wake-up. The caller performs the public recv and owns the resulting
        // peer/error handling, matching the .NET poller contract.
        return events.hasEvent(index, PollEventFlags.POLLIN)
                || events.hasEvent(index, PollEventFlags.POLLERR)
                || events.hasEvent(index, PollEventFlags.POLLPRI);
    }

    @Override
    public void close() {
        signal();
        synchronized (this) {
            if (closed) return;
            poller.close();
            if (wakeTimer != null) wakeTimer.close();
            closed = true;
        }
    }
}
