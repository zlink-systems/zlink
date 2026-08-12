/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.Socket;
import java.time.Duration;
import java.util.List;
import java.util.Objects;

public final class PerfSocketPollSet implements AutoCloseable {
    private final Socket[] sockets;
    private final int[] currentMasks;
    private final Poller poller;
    private final PollEvents events;
    private int readyCount;

    private PerfSocketPollSet(List<Socket> sockets,
                              PollEventFlags... initialEvents) {
        this.sockets = sockets.toArray(Socket[]::new);
        this.currentMasks = new int[this.sockets.length];
        this.poller = Zlink.createPoller();
        this.events = new PollEvents(Math.max(1, this.sockets.length));
        readyCount = 0;
        for (int i = 0; i < this.sockets.length; i++) {
            Socket socket = Objects.requireNonNull(this.sockets[i], "socket");
            poller.add(socket, i, initialEvents);
            currentMasks[i] = mask(initialEvents);
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
        if (offset < 0 || offset >= readyCount) {
            throw new IndexOutOfBoundsException("ready offset " + offset);
        }
        return (int) events.slot(offset);
    }

    public int readyMaskAt(int offset) {
        if (offset < 0 || offset >= readyCount) {
            throw new IndexOutOfBoundsException("ready offset " + offset);
        }
        return events.revents(offset);
    }

    public boolean readyHasEventAt(int offset, PollEventFlags event) {
        return (readyMaskAt(offset) & maskOne(event)) != 0;
    }

    public int poll(int timeoutMs) {
        readyCount = 0;
        readyCount = poller.wait(events, Duration.ofMillis(timeoutMs));
        return readyCount;
    }

    @Override
    public void close() {
        poller.close();
    }

    private void checkIndex(int index) {
        if (index < 0 || index >= sockets.length) {
            throw new IndexOutOfBoundsException("index " + index);
        }
    }

    private static int mask(PollEventFlags... events) {
        int mask = 0;
        if (events == null) {
            return mask;
        }
        for (PollEventFlags event : events) {
            mask |= switch (event) {
                case POLLIN -> PollEventFlags.POLLIN.mask();
                case POLLOUT -> PollEventFlags.POLLOUT.mask();
                case POLLERR -> PollEventFlags.POLLERR.mask();
                case POLLPRI -> PollEventFlags.POLLPRI.mask();
                case POLLCOMPLETION -> PollEventFlags.POLLCOMPLETION.mask();
            };
        }
        return mask;
    }

    private static int maskOne(PollEventFlags event) {
        return switch (event) {
            case POLLIN -> PollEventFlags.POLLIN.mask();
            case POLLOUT -> PollEventFlags.POLLOUT.mask();
            case POLLERR -> PollEventFlags.POLLERR.mask();
            case POLLPRI -> PollEventFlags.POLLPRI.mask();
            case POLLCOMPLETION -> PollEventFlags.POLLCOMPLETION.mask();
        };
    }

}
