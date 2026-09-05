/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.eventing;

import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.MonitorStatus;

import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.NativeMonitorStatuses;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.Objects;
import java.util.Optional;

public final class NativeMonitorSocket implements SocketMonitor {
    private MemorySegment handle;
    private final boolean own;

    static {
        InternalAccess.register(new InternalAccess.MonitorAccess() {
            @Override
            public SocketMonitor create(MemorySegment handle, boolean own) {
                return new NativeMonitorSocket(handle, own);
            }

            @Override
            public MemorySegment handle(SocketMonitor monitor) {
                return ((NativeMonitorSocket) monitor).handle();
            }
        });
    }

    NativeMonitorSocket(MemorySegment handle, boolean own) {
        this.handle = handle;
        this.own = own;
    }

    public MonitorEvent recv() {
        return recv(RecvFlags.NONE);
    }

    public MonitorEvent recv(RecvFlags flags) {
        Objects.requireNonNull(flags, "flags");
        ensureOpen();
        try {
            return Native.monitorRecv(handle, flags.value());
        } catch (ZlinkRecvException ex) {
            if (flags == RecvFlags.DONT_WAIT
                && ex.getResult() == RecvResult.NO_DATA) {
                return null;
            }
            throw ex;
        }
    }

    Optional<MonitorEvent> recvNoWait() {
        ensureOpen();
        return Optional.ofNullable(recv(RecvFlags.DONT_WAIT));
    }

    public MonitorStatus status() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = arena.allocate(
              NativeLayouts.MONITOR_SNAPSHOT_LAYOUT);
            int rc = Native.monitorStatus(handle, out);
            if (rc != 0)
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            return NativeMonitorStatuses.fromNative(out);
        }
    }

    MemorySegment handle() {
        return handle;
    }

    @Override
    public void close() {
        if (handle == null || handle.address() == 0)
            return;
        if (own) {
            Native.monitorClose(handle);
        }
        handle = MemorySegment.NULL;
    }

    private void ensureOpen() {
        if (handle == null || handle.address() == 0)
            throw new IllegalStateException("monitor socket is closed");
    }
}
