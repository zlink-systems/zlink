/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.List;
import java.util.Objects;

public final class PerfSocketPollSet implements AutoCloseable {
    private final Socket[] sockets;
    private final int[] currentMasks;
    private final Arena arena;
    private final MemorySegment events;
    private MemorySegment poller;
    private int readyCount;
    private boolean closed;

    // zlink_poller_event_t is part of the Core C ABI used by the C reference
    // benchmark. The perf harness reads only the same user-data and event-mask
    // fields that C reads before it DONT_WAIT-drains the ready socket.
    private static final long POLLER_EVENT_SIZE = 48;
    private static final long EVENT_USER_DATA_OFFSET = 32;
    private static final long EVENT_EVENTS_OFFSET = 40;

    private PerfSocketPollSet(List<Socket> sockets,
                              PollEventFlags... initialEvents) {
        this.sockets = sockets.toArray(Socket[]::new);
        this.currentMasks = new int[this.sockets.length];
        this.arena = Arena.ofShared();
        this.events = arena.allocate(POLLER_EVENT_SIZE
            * Math.max(1, this.sockets.length), ValueLayout.ADDRESS.byteAlignment());
        this.poller = Native.pollerNew();
        if (poller.address() == 0) {
            arena.close();
            throw nativePollerFailure("create");
        }
        readyCount = 0;
        for (int i = 0; i < this.sockets.length; i++) {
            Socket socket = Objects.requireNonNull(this.sockets[i], "socket");
            int initialMask = mask(initialEvents);
            if (Native.pollerAdd(poller, InternalAccess.socketHandle(socket),
                MemorySegment.ofAddress(i), initialMask) != 0) {
                close();
                throw nativePollerFailure("add");
            }
            currentMasks[i] = initialMask;
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
        if (Native.pollerModify(poller, InternalAccess.socketHandle(sockets[index]),
            newMask) != 0) {
            throw nativePollerFailure("modify");
        }
        currentMasks[index] = newMask;
    }

    public int readyCount() {
        return readyCount;
    }

    public int readyIndexAt(int offset) {
        if (offset < 0 || offset >= readyCount) {
            throw new IndexOutOfBoundsException("ready offset " + offset);
        }
        return (int) events.get(ValueLayout.ADDRESS,
            eventOffset(offset, EVENT_USER_DATA_OFFSET)).address();
    }

    public int readyMaskAt(int offset) {
        if (offset < 0 || offset >= readyCount) {
            throw new IndexOutOfBoundsException("ready offset " + offset);
        }
        return events.get(ValueLayout.JAVA_SHORT,
            eventOffset(offset, EVENT_EVENTS_OFFSET));
    }

    public boolean readyHasEventAt(int offset, PollEventFlags event) {
        return (readyMaskAt(offset) & maskOne(event)) != 0;
    }

    public int poll(int timeoutMs) {
        readyCount = 0;
        // HOT PATH: this is intentionally the C-equivalent poller path. Do
        // not materialize each native ready event as a public PollEvents entry
        // before the caller drains the corresponding socket.
        readyCount = Native.pollerWait(poller, events, sockets.length,
            timeoutMs);
        if (readyCount < 0) {
            throw nativePollerFailure("wait");
        }
        return readyCount;
    }

    @Override
    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        if (poller.address() != 0) {
            Native.pollerDestroy(poller);
            poller = MemorySegment.NULL;
        }
        arena.close();
    }

    private void checkIndex(int index) {
        if (index < 0 || index >= sockets.length) {
            throw new IndexOutOfBoundsException("index " + index);
        }
    }

    private static long eventOffset(int index, long fieldOffset) {
        return (long) index * POLLER_EVENT_SIZE + fieldOffset;
    }

    private static IllegalStateException nativePollerFailure(String operation) {
        return new IllegalStateException("native poller " + operation
            + " failed: errno=" + Native.errno());
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
