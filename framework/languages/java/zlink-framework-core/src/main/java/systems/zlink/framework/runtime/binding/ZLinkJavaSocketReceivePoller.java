package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.PollSourceKind;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.sockets.Socket;

import java.time.Duration;
import java.util.Objects;

/**
 * Owns the public zlink Poller used to guard one Framework socket receive.
 *
 * <p>The Framework asks this object whether the socket is readable before it calls the socket's
 * public receive method. The poller and its reusable event storage remain binding-owned, so receive
 * loops do not know binding details.
 */
final class ZLinkJavaSocketReceivePoller implements AutoCloseable {
    private static final long SOCKET_SLOT = 1L;
    static final int READABLE = 1;
    static final int ROUTE_CHANGED = 2;

    private final Socket socket;
    private final Poller poller;
    private final boolean ownsCompletionQueue;
    private final boolean observesRoutes;
    private final PollEvents events = new PollEvents(1);
    private volatile boolean registered;
    private boolean closed;

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
        Objects.requireNonNull(socket, "socket");
        this.socket = socket;
        this.ownsCompletionQueue = ownsCompletionQueue;
        this.observesRoutes = observesRoutes;
        poller = Zlink.createPoller();
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
            registered = true;
        }
    }

    synchronized boolean waitForReadable(Duration timeout) {
        return (waitForReady(timeout) & READABLE) != 0;
    }

    /** Waits once and returns the {@link #READABLE} and {@link #ROUTE_CHANGED} bits it reported. */
    synchronized int waitForReady(Duration timeout) {
        if (closed) {
            return 0;
        }
        ensureRegistered();
        int count = poller.wait(events, timeout);
        int ready = 0;
        for (int index = 0; index < count; index++) {
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

    private static boolean isReadable(PollEvents events, int index) {
        // A socket close or exceptional receive condition is still a receive
        // wake-up. The caller performs the public recv and owns the resulting
        // peer/error handling, matching the .NET poller contract.
        return events.hasEvent(index, PollEventFlags.POLLIN)
                || events.hasEvent(index, PollEventFlags.POLLERR)
                || events.hasEvent(index, PollEventFlags.POLLPRI);
    }

    @Override
    public synchronized void close() {
        if (closed) {
            return;
        }
        closed = true;
        poller.close();
    }
}
