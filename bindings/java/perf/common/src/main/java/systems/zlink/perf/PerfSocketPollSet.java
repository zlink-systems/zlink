/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ErrorCategory;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.List;
import java.util.Objects;

public final class PerfSocketPollSet implements AutoCloseable {
    private static final int MASK_POLLIN = 1;
    private static final int MASK_POLLOUT = 2;
    private static final int MASK_POLLERR = 4;
    private static final int MASK_POLLPRI = 8;
    private static final int MASK_POLLCOMPLETION = 32;
    private static final long POLLER_EVENT_SIZE = 48;
    private static final long EVENT_USER_DATA_OFFSET = 32;
    private static final long EVENT_EVENTS_OFFSET = 40;
    private final Socket[] sockets;
    private final int[] currentMasks;
    private final Arena nativeArena = Arena.ofShared();
    private final MemorySegment poller;
    private final MemorySegment nativeEvents;
    private final MemorySegment errorOut;
    private int readyCount;

    private PerfSocketPollSet(List<Socket> sockets,
                              PollEventFlags... initialEvents) {
        this.sockets = sockets.toArray(Socket[]::new);
        this.currentMasks = new int[this.sockets.length];
        this.poller = Native.pollerNew();
        if (poller == null || poller.address() == 0) {
            nativeArena.close();
            throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
        }
        this.nativeEvents = nativeArena.allocate(
            POLLER_EVENT_SIZE * Math.max(1, this.sockets.length),
            ValueLayout.ADDRESS.byteAlignment());
        this.errorOut = nativeArena.allocate(ValueLayout.JAVA_INT);
        readyCount = 0;
        int initialMask = mask(initialEvents);
        for (int i = 0; i < this.sockets.length; i++) {
            Socket socket = Objects.requireNonNull(this.sockets[i], "socket");
            int rc = Native.pollerAdd(poller, InternalAccess.socketHandle(socket),
                MemorySegment.ofAddress(i), initialMask);
            if (rc != 0) {
                close();
                throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
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
        int rc = Native.pollerModify(poller, InternalAccess.socketHandle(sockets[index]),
            newMask);
        if (rc != 0) {
            throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
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
        return (int) nativeEvents.get(ValueLayout.ADDRESS,
            eventOffset(offset, EVENT_USER_DATA_OFFSET)).address();
    }

    public int readyMaskAt(int offset) {
        if (offset < 0 || offset >= readyCount) {
            throw new IndexOutOfBoundsException("ready offset " + offset);
        }
        return nativeEvents.get(ValueLayout.JAVA_SHORT,
            eventOffset(offset, EVENT_EVENTS_OFFSET));
    }

    public boolean readyHasEventAt(int offset, PollEventFlags event) {
        return (readyMaskAt(offset) & maskOne(event)) != 0;
    }

    public int poll(int timeoutMs) {
        readyCount = 0;
        try {
            readyCount = Native.pollerWait(poller, nativeEvents, sockets.length,
                timeoutMs, errorOut);
        } catch (ZlinkException ex) {
            int errno = ex.getNativeErrno();
            if (PerfErrno.isRetryableSend(errno)) {
                return 0;
            }
            throw ex;
        }
        if (readyCount < 0) {
            throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
        }
        return readyCount;
    }

    @Override
    public void close() {
        try {
            if (poller.address() != 0) {
                int rc = Native.pollerDestroy(poller);
                if (rc != 0) {
                    throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
                }
            }
        } finally {
            nativeArena.close();
        }
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
                case POLLIN -> 1;
                case POLLOUT -> MASK_POLLOUT;
                case POLLERR -> MASK_POLLERR;
                case POLLPRI -> MASK_POLLPRI;
                case POLLCOMPLETION -> MASK_POLLCOMPLETION;
            };
        }
        return mask;
    }

    private static int maskOne(PollEventFlags event) {
        return switch (event) {
            case POLLIN -> MASK_POLLIN;
            case POLLOUT -> MASK_POLLOUT;
            case POLLERR -> MASK_POLLERR;
            case POLLPRI -> MASK_POLLPRI;
            case POLLCOMPLETION -> MASK_POLLCOMPLETION;
        };
    }

    private static long eventOffset(int index, long fieldOffset) {
        return (long) index * POLLER_EVENT_SIZE + fieldOffset;
    }

}
