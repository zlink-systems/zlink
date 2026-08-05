/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.eventing;

import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.PollSourceKind;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.ZlinkTimer;
import systems.zlink.internal.ContractAccess;

import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.errors.ZlinkException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.RequestProgressPump;
import systems.zlink.internal.DurationConversions;

public final class NativePoller implements Poller {
    private final List<PollItem> items = new ArrayList<>();
    private final Map<Long, Integer> socketIndexes = new HashMap<>();
    private MemorySegment handle;
    private Arena waitArena;
    private MemorySegment waitEvents = MemorySegment.NULL;
    private MemorySegment waitErrorOut = MemorySegment.NULL;
    private int waitEventsCapacity;

    public static Poller create() {
        return new NativePoller();
    }

    NativePoller() {
        handle = Native.pollerNew();
        if (handle == null || handle.address() == 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
    }

    public void add(Socket socket, long slot, PollEventFlags... events) {
        addSocket(socket, combine(events), slot);
    }

    public void addFd(int fd, long slot, PollEventFlags... events) {
        ensureOpen();
        validateSlot(slot);
        PollItem item = PollItem.fd(fd, combine(events), slot);
        int rc = Native.pollerAddFd(handle, fd, item.userData(), item.events);
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        items.add(item);
    }

    public void add(ZlinkTimer timer, long slot) {
        ensureOpen();
        Objects.requireNonNull(timer, "timer");
        validateSlot(slot);
        PollItem item = PollItem.timer(InternalAccess.timerHandle(timer), slot);
        int rc = Native.pollerAddZlinkTimer(handle, InternalAccess.timerHandle(timer), item.userData());
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        items.add(item);
    }

    public void modify(Socket socket, PollEventFlags... events) {
        ensureOpen();
        Objects.requireNonNull(socket, "socket");
        int index = findSocket(InternalAccess.socketHandle(socket));
        if (index < 0)
            throw new IllegalArgumentException("socket is not registered");
        int mask = combine(events);
        PollItem item = items.get(index);
        boolean hadExternalProgress = hasExternalProgress(item);
        boolean needsExternalProgress = hasCompletion(mask);
        if (needsExternalProgress && !hadExternalProgress) {
            RequestProgressPump.acquireExternalProgress(item.handle);
        }
        int rc;
        try {
            rc = Native.pollerModify(handle, InternalAccess.socketHandle(socket), mask);
        } catch (RuntimeException | Error failure) {
            if (needsExternalProgress && !hadExternalProgress) {
                RequestProgressPump.releaseExternalProgress(item.handle);
            }
            throw failure;
        }
        if (rc != 0) {
            if (needsExternalProgress && !hadExternalProgress) {
                RequestProgressPump.releaseExternalProgress(item.handle);
            }
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        if (hadExternalProgress && !needsExternalProgress) {
            RequestProgressPump.releaseExternalProgress(item.handle);
        }
        item.events = mask;
    }

    public void modifyFd(int fd, PollEventFlags... events) {
        ensureOpen();
        int index = findFd(fd);
        if (index < 0)
            throw new IllegalArgumentException("fd is not registered");
        int mask = combine(events);
        int rc = Native.pollerModifyFd(handle, fd, mask);
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        items.get(index).events = mask;
    }

    public boolean remove(Socket socket) {
        ensureOpen();
        Objects.requireNonNull(socket, "socket");
        int index = findSocket(InternalAccess.socketHandle(socket));
        if (index < 0)
            return false;
        int rc = Native.pollerRemove(handle, InternalAccess.socketHandle(socket));
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        PollItem removed = items.remove(index);
        unregisterExternalProgress(removed);
        socketIndexes.remove(removed.handle.address());
        refreshIndexesFrom(index);
        return true;
    }

    public boolean remove(int fd) {
        ensureOpen();
        int index = findFd(fd);
        if (index < 0)
            return false;
        int rc = Native.pollerRemoveFd(handle, fd);
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        items.remove(index);
        return true;
    }

    public boolean remove(ZlinkTimer timer) {
        ensureOpen();
        Objects.requireNonNull(timer, "timer");
        int index = findZlinkTimer(InternalAccess.timerHandle(timer));
        if (index < 0)
            return false;
        int rc = Native.pollerRemoveZlinkTimer(handle, InternalAccess.timerHandle(timer));
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        items.remove(index);
        return true;
    }

    public void clear() {
        ensureOpen();
        int rc = Native.pollerDestroy(handle);
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        unregisterAllExternalProgress();
        handle = Native.pollerNew();
        if (handle == null || handle.address() == 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        items.clear();
        socketIndexes.clear();
    }

    public int size() {
        ensureOpen();
        int rc = Native.pollerSize(handle);
        if (rc < 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        return rc;
    }

    public int wait(PollEvents events, Duration timeout) {
        ensureOpen();
        Objects.requireNonNull(events, "events");
        if (items.isEmpty()) {
            ContractAccess.pollEventsMarkReadyCount(events, 0);
            return 0;
        }
        MemorySegment nativeEvents = waitEvents(events.capacity());
        int readyCount = Native.pollerWait(handle, nativeEvents, events.capacity(),
            DurationConversions.toIntMillis(timeout, "timeout"), waitErrorOut);
        if (readyCount < 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        for (int i = 0; i < readyCount; i++) {
            ContractAccess.pollEventsMarkEvent(events, i,
                NativePollEvents.sourceKindValue(nativeEvents, i),
                NativePollEvents.slot(nativeEvents, i),
                NativePollEvents.revents(nativeEvents, i),
                NativePollEvents.fd(nativeEvents, i));
        }
        ContractAccess.pollEventsMarkReadyCount(events, readyCount);
        return readyCount;
    }

    @Override
    public void close() {
        if (handle == null || handle.address() == 0)
            return;
        Native.pollerDestroy(handle);
        handle = MemorySegment.NULL;
        unregisterAllExternalProgress();
        items.clear();
        closeWaitArena();
    }

    private MemorySegment waitEvents(int capacity) {
        if (waitArena != null && waitEventsCapacity >= capacity)
            return waitEvents;

        closeWaitArena();
        waitArena = Arena.ofShared();
        // HOT PATH: Poller.wait can run once per receive/send loop iteration.
        // Keep native event and error-out storage with the poller so public
        // wait calls do not allocate a new Arena and MemorySegment each time.
        waitEvents = NativePollEvents.create(waitArena, capacity);
        waitErrorOut = waitArena.allocate(ValueLayout.JAVA_INT);
        waitEventsCapacity = capacity;
        return waitEvents;
    }

    private void closeWaitArena() {
        if (waitArena == null)
            return;
        try {
            waitArena.close();
        } finally {
            waitArena = null;
            waitEvents = MemorySegment.NULL;
            waitErrorOut = MemorySegment.NULL;
            waitEventsCapacity = 0;
        }
    }

    private void addSocket(Socket socket, int events, long slot) {
        ensureOpen();
        Objects.requireNonNull(socket, "socket");
        validateSlot(slot);
        PollItem item = PollItem.socket(InternalAccess.socketHandle(socket), events, slot);
        boolean externalProgress = hasExternalProgress(item);
        if (externalProgress) {
            // Reserve the completion owner before native registration. The
            // binding fallback pump is stopped by this call, so the public
            // poller becomes the only completion consumer for this socket.
            RequestProgressPump.acquireExternalProgress(item.handle);
        }
        int rc;
        try {
            rc = Native.pollerAdd(handle, InternalAccess.socketHandle(socket), item.userData(),
                events);
        } catch (RuntimeException | Error failure) {
            if (externalProgress) {
                RequestProgressPump.releaseExternalProgress(item.handle);
            }
            throw failure;
        }
        if (rc != 0) {
            if (externalProgress) {
                RequestProgressPump.releaseExternalProgress(item.handle);
            }
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        socketIndexes.putIfAbsent(InternalAccess.socketHandle(socket).address(), items.size());
        items.add(item);
    }

    private void ensureOpen() {
        if (handle == null || handle.address() == 0)
            throw new IllegalStateException("poller is closed");
    }

    private static void unregisterExternalProgress(PollItem item) {
        if (hasExternalProgress(item)) {
            RequestProgressPump.releaseExternalProgress(item.handle);
        }
    }

    private static boolean hasExternalProgress(PollItem item) {
        return item.kind == PollSourceKind.SOCKET && hasCompletion(item.events);
    }

    private static boolean hasCompletion(int events) {
        return (events & PollEventFlags.POLLCOMPLETION.mask()) != 0;
    }

    private void unregisterAllExternalProgress() {
        for (PollItem item : items) {
            unregisterExternalProgress(item);
        }
    }

    private static void validateSlot(long slot) {
        if (slot < 0)
            throw new IllegalArgumentException("slot must be >= 0");
    }

    private static int combine(PollEventFlags... flags) {
        int mask = 0;
        if (flags != null) {
            for (PollEventFlags flag : flags) {
                if (flag != null) {
                    mask |= flag.mask();
                }
            }
        }
        return mask;
    }

    private int findSocket(MemorySegment socketHandle) {
        return socketIndexes.getOrDefault(socketHandle.address(), -1);
    }

    private int findFd(int fd) {
        for (int i = 0; i < items.size(); i++) {
            PollItem item = items.get(i);
            if (item.kind == PollSourceKind.FD && item.fd == fd)
                return i;
        }
        return -1;
    }

    private void refreshIndexesFrom(int start) {
        for (int i = start; i < items.size(); i++) {
            PollItem item = items.get(i);
            if (item.kind != PollSourceKind.SOCKET)
                continue;
            socketIndexes.put(item.handle.address(), i);
        }
    }

    private int findZlinkTimer(MemorySegment timerHandle) {
        for (int i = 0; i < items.size(); i++) {
            PollItem item = items.get(i);
            if (item.kind == PollSourceKind.TIMER
                && item.handle.address() == timerHandle.address()) {
                return i;
            }
        }
        return -1;
    }

    private static final class PollItem {
        private final PollSourceKind kind;
        private final MemorySegment handle;
        private final int fd;
        private int events;
        private final long slot;
        private PollItem(PollSourceKind kind, MemorySegment handle, int fd,
                         int events, long slot) {
            this.kind = kind;
            this.handle = handle;
            this.fd = fd;
            this.events = events;
            this.slot = slot;
        }

        static PollItem socket(MemorySegment handle, int events, long slot) {
            return new PollItem(PollSourceKind.SOCKET, handle, 0, events,
                slot);
        }

        static PollItem fd(int fd, int events, long slot) {
            return new PollItem(PollSourceKind.FD, MemorySegment.NULL, fd,
                events, slot);
        }

        static PollItem timer(MemorySegment handle, long slot) {
            return new PollItem(PollSourceKind.TIMER, handle, 0, 0, slot);
        }

        MemorySegment userData() {
            return MemorySegment.ofAddress(slot);
        }
    }
}
