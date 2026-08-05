/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.eventing;

import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.MonitorStatus;
import systems.zlink.contracts.eventing.SocketMonitorHandler;

import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeCallbackSupport;
import systems.zlink.runtime.nativeapi.NativeHelpers;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.NativeMonitorStatuses;
import systems.zlink.runtime.nativeapi.RuntimeResources;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.ExecutorService;

public final class NativeMonitorSocket implements SocketMonitor {
    private static final Linker LINKER = Linker.nativeLinker();
    private static final FunctionDescriptor FD_MONITOR_CALLBACK =
      FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.ADDRESS);

    private MemorySegment handle;
    private final boolean own;
    private SocketMonitorHandler eventHandler;
    private Arena callbackArena;
    private MemorySegment callbackStub = MemorySegment.NULL;
    private final NativeCallbackSupport callbacks =
        new NativeCallbackSupport("zlink-monitor-callback");

    static {
        InternalAccess.register((InternalAccess.MonitorAccess)
            NativeMonitorSocket::new);
    }

    NativeMonitorSocket(MemorySegment handle, boolean own) {
        this.handle = handle;
        this.own = own;
    }

    public void onEvent(SocketMonitorHandler handler) {
        Objects.requireNonNull(handler, "handler");
        ensureOpen();
        callbacks.ensureNoFailure();
        ExecutorService previousExecutor = callbacks.executor();
        ExecutorService executor = callbacks.replaceExecutor();
        Arena arena = Arena.ofShared();
        MemorySegment stub = LINKER.upcallStub(callbackHandle(
          "handleEventCallback", MethodType.methodType(void.class,
            MemorySegment.class, MemorySegment.class)),
          FD_MONITOR_CALLBACK, arena);
        boolean success = false;
        try {
            int rc = Native.monitorHandler(handle, stub, MemorySegment.NULL);
            if (rc != 0) {
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.HANDLER);
            }
            success = true;
            RuntimeResources.closeArena(callbackArena);
            callbackArena = arena;
            callbackStub = stub;
            eventHandler = handler;
            RuntimeResources.shutdownExecutor(previousExecutor);
        } finally {
            if (!success) {
                callbacks.restoreExecutor(previousExecutor, executor);
                RuntimeResources.closeArena(arena);
            }
        }
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
        eventHandler = null;
        callbacks.close();
        if (own) {
            Native.monitorClose(handle);
        }
        RuntimeResources.closeArena(callbackArena);
        callbackArena = null;
        callbackStub = MemorySegment.NULL;
        handle = MemorySegment.NULL;
    }

    private void ensureOpen() {
        if (handle == null || handle.address() == 0)
            throw new IllegalStateException("monitor socket is closed");
        callbacks.ensureNoFailure();
    }

    private MethodHandle callbackHandle(String name, MethodType type) {
        try {
            return MethodHandles.lookup().findVirtual(NativeMonitorSocket.class, name,
              type).bindTo(this);
        } catch (ReflectiveOperationException ex) {
            throw new IllegalStateException("failed to bind callback " + name,
              ex);
        }
    }

    private void handleEventCallback(MemorySegment event,
                                     MemorySegment userdata) {
        SocketMonitorHandler handler = eventHandler;
        ExecutorService executor = callbacks.executor();
        if (handler == null || executor == null || executor.isShutdown())
            return;
        try {
            MemorySegment evt = event.reinterpret(
              NativeLayouts.MONITOR_EVENT_LAYOUT.byteSize());
            long eventValue = evt.get(ValueLayout.JAVA_LONG,
              NativeLayouts.MONITOR_EVENT_OFFSET);
            long value = evt.get(ValueLayout.JAVA_LONG,
              NativeLayouts.MONITOR_VALUE_OFFSET);
            int routingSize = evt.get(ValueLayout.JAVA_BYTE,
              NativeLayouts.MONITOR_ROUTING_OFFSET
                + NativeLayouts.ROUTING_ID_SIZE_OFFSET) & 0xFF;
            byte[] routing = new byte[routingSize];
            if (routingSize > 0) {
                MemorySegment.copy(evt,
                  NativeLayouts.MONITOR_ROUTING_OFFSET
                    + NativeLayouts.ROUTING_ID_DATA_OFFSET,
                  MemorySegment.ofArray(routing), 0, routingSize);
            }
            String local = NativeHelpers.fromCString(evt.asSlice(
              NativeLayouts.MONITOR_LOCAL_OFFSET, 256), 256);
            String remote = NativeHelpers.fromCString(evt.asSlice(
              NativeLayouts.MONITOR_REMOTE_OFFSET, 256), 256);
            MonitorEvent monitorEvent = new MonitorEvent(
              MonitorEventType.fromValue(eventValue), value,
              routingSize == 0 ? Optional.empty()
                : Optional.of(InternalAccess.routingIdFromTrusted(routing)),
              local, remote);
            executor.execute(() -> dispatchEvent(handler, monitorEvent));
        } catch (RuntimeException ex) {
            callbacks.recordFailure(ex);
        }
    }

    private void dispatchEvent(SocketMonitorHandler handler,
                               MonitorEvent event) {
        InternalAccess.enterCallback();
        try {
            handler.onEvent(event);
        } catch (RuntimeException ex) {
            callbacks.recordFailure(ex);
        } finally {
            InternalAccess.leaveCallback();
        }
    }
}
