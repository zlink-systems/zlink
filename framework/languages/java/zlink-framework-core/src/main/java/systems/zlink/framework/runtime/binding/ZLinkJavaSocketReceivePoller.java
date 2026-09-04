package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import java.util.Objects;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.PollSourceKind;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.sockets.Socket;

/**
 * Owns the public zlink Poller used to guard one Framework socket receive.
 *
 * <p>The Framework asks this object whether the socket is readable before it
 * calls the socket's public receive method. The poller and its reusable event
 * storage remain binding-owned, so receive loops do not know binding details.</p>
 */
final class ZLinkJavaSocketReceivePoller implements AutoCloseable {
    private static final long SOCKET_SLOT = 1L;

    private final Socket socket;
    private final Poller poller;
    private final boolean ownsCompletionQueue;
    private final PollEvents events = new PollEvents(1);
    private boolean registered;
    private boolean closed;

    ZLinkJavaSocketReceivePoller(Socket socket) {
        this(socket, true);
    }

    ZLinkJavaSocketReceivePoller(
        Socket socket,
        boolean ownsCompletionQueue) {
        Objects.requireNonNull(socket, "socket");
        this.socket = socket;
        this.ownsCompletionQueue = ownsCompletionQueue;
        poller = Zlink.createPoller();
    }

    synchronized void ensureRegistered() {
        if (closed || registered) {
            return;
        }
        // Framework creates the wrapper before it applies routing options
        // and calls bind/connect. Register the fully configured socket when
        // the receive owner first needs readiness. For socket kinds that
        // support it, the binding owns async DONTWAIT retry and completion
        // draining on this public poller.
        if (ownsCompletionQueue) {
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

    synchronized boolean waitForReadable(Duration timeout) {
        if (closed) {
            return false;
        }
        ensureRegistered();
        int count = poller.wait(events, timeout);
        for (int index = 0; index < count; index++) {
            if (events.sourceKind(index) == PollSourceKind.SOCKET
                && events.slot(index) == SOCKET_SLOT
                && isReadable(events, index)) {
                return true;
            }
        }
        return false;
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
