/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.sockets.Socket;
import java.time.Duration;
import java.util.List;
import java.util.Objects;

/**
 * Reusable poll state for the multi-socket perf scenarios.
 *
 * <p>This helper stays on the binding's public contract. The public poller
 * owns completion-progress coordination, so a request callback cannot create
 * a second completion consumer while this poll set waits for the same socket.</p>
 */
public final class PerfSocketPollSet implements AutoCloseable {
    private final Socket[] sockets;
    private final int[] currentMasks;
    private final Poller poller;
    private final PollEvents events;
    private int readyCount;
    private boolean closed;

    private PerfSocketPollSet(List<Socket> sockets,
                              PollEventFlags... initialEvents) {
        this.sockets = sockets.toArray(Socket[]::new);
        this.currentMasks = new int[this.sockets.length];
        this.poller = Zlink.createPoller();
        this.events = new PollEvents(Math.max(1, this.sockets.length));
        try {
            int initialMask = mask(initialEvents);
            for (int i = 0; i < this.sockets.length; i++) {
                Socket socket = Objects.requireNonNull(this.sockets[i], "socket");
                poller.add(socket, i, initialEvents);
                currentMasks[i] = initialMask;
            }
        } catch (RuntimeException | Error failure) {
            poller.close();
            throw failure;
        }
    }

    public static PerfSocketPollSet fromSockets(List<Socket> sockets,
                                                PollEventFlags... initialEvents) {
        Objects.requireNonNull(sockets, "sockets");
        return new PerfSocketPollSet(sockets, initialEvents);
    }

    public void setEvents(int index, PollEventFlags... newEvents) {
        checkIndex(index);
        int newMask = mask(newEvents);
        if (currentMasks[index] == newMask) {
            return;
        }
        poller.modify(sockets[index], newEvents);
        currentMasks[index] = newMask;
    }

    public int readyCount() {
        return readyCount;
    }

    public int readyIndexAt(int offset) {
        checkReadyOffset(offset);
        return (int) events.slot(offset);
    }

    public int readyMaskAt(int offset) {
        checkReadyOffset(offset);
        return events.revents(offset);
    }

    public boolean readyHasEventAt(int offset, PollEventFlags event) {
        checkReadyOffset(offset);
        return events.hasEvent(offset, event);
    }

    public int poll(int timeoutMs) {
        if (timeoutMs < -1) {
            throw new IllegalArgumentException("timeoutMs must be >= -1");
        }
        // HOT PATH: preserve the C runner's one-poller wait/drain sequence.
        // PollEvents owns preallocated result arrays, and the public Poller
        // reuses its native event storage across waits.
        readyCount = poller.wait(events, Duration.ofMillis(timeoutMs));
        return readyCount;
    }

    @Override
    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        poller.close();
    }

    private void checkIndex(int index) {
        if (index < 0 || index >= sockets.length) {
            throw new IndexOutOfBoundsException("index " + index);
        }
    }

    private void checkReadyOffset(int offset) {
        if (offset < 0 || offset >= readyCount) {
            throw new IndexOutOfBoundsException("ready offset " + offset);
        }
    }

    private static int mask(PollEventFlags... events) {
        int mask = 0;
        if (events == null) {
            return mask;
        }
        for (PollEventFlags event : events) {
            mask |= event.mask();
        }
        return mask;
    }
}
