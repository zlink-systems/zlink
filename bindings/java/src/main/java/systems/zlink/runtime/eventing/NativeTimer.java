/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.eventing;

import systems.zlink.contracts.eventing.ZlinkTimer;

import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.time.Duration;
import java.util.Objects;

public final class NativeTimer implements ZlinkTimer {
    private final boolean ownsHandle;
    private MemorySegment handle;
    private boolean closed;

    static {
        InternalAccess.register((InternalAccess.TimerAccess)
            new InternalAccess.TimerAccess() {
                @Override
                public MemorySegment handle(ZlinkTimer timer) {
                    return ((NativeTimer) timer).handle();
                }

                @Override
                public ZlinkTimer fromBorrowedHandle(MemorySegment handle) {
                    return NativeTimer.fromBorrowedHandle(handle);
                }
            });
    }

    public static ZlinkTimer create() {
        return new NativeTimer();
    }

    NativeTimer() {
        this(Native.timerNew(), true);
    }

    private NativeTimer(MemorySegment handle, boolean ownsHandle) {
        this.handle = handle;
        this.ownsHandle = ownsHandle;
        if (handle == null || handle.address() == 0) {
            throw new ZlinkConfigException(ConfigResult.INVALID_HANDLE);
        }
    }

    static ZlinkTimer fromBorrowedHandle(MemorySegment handle) {
        return new NativeTimer(handle, false);
    }

    MemorySegment handle() {
        ensureOpen();
        return handle;
    }

    public void start(Duration interval, long repeatCount) {
        startNanos(toNanos(interval, "interval"), repeatCount);
    }

    void startNanos(long intervalNs, long repeatCount) {
        ensureOpen();
        int rc = Native.timerStart(handle, intervalNs, repeatCount);
        if (rc != 0) {
            throw new ZlinkConfigException(ConfigResult.INVALID_ARGUMENT);
        }
    }

    public void stop() {
        ensureOpen();
        int rc = Native.timerStop(handle);
        if (rc != 0) {
            throw new ZlinkConfigException(ConfigResult.INVALID_HANDLE);
        }
    }

    public long recv() {
        ensureOpen();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment fireCount = arena.allocate(ValueLayout.JAVA_LONG);
            int rc = Native.timerRecv(handle, fireCount);
            if (rc == 0) {
                return fireCount.get(ValueLayout.JAVA_LONG, 0);
            }
            throw new ZlinkRecvException(RecvResult.NO_DATA);
        }
    }

    private static long toNanos(Duration duration, String name) {
        Objects.requireNonNull(duration, name);
        try {
            return duration.toNanos();
        } catch (ArithmeticException ex) {
            throw new IllegalArgumentException(name + " is too large", ex);
        }
    }

    @Override
    public void close() {
        if (closed || handle == null || handle.address() == 0) {
            return;
        }
        closed = true;
        if (ownsHandle) {
            Native.timerDestroy(handle);
        }
        handle = MemorySegment.NULL;
    }

    private void ensureOpen() {
        if (handle == null || handle.address() == 0) {
            throw new IllegalStateException("timer is closed");
        }
    }

}
