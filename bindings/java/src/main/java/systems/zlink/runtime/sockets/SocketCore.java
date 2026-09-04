/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;
import systems.zlink.internal.sockets.SocketOptionKey;
import systems.zlink.internal.sockets.SocketOptions;
import systems.zlink.internal.sockets.SocketOptionValueType;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.sockets.*;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.runtime.eventing.NativeMonitorSocket;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.RuntimeResources;
import systems.zlink.runtime.nativeapi.CompletionDispatcher;

final class SocketCore {
    /**
     * Tracks whether the current thread is executing inside a native callback
     * (recv handler, subscribe handler, or stream packet handler).
     *
     * <p>Blocking sends from a callback can deadlock the socket I/O thread.
     * The public blocking send APIs therefore reject callback usage explicitly
     * instead of silently downgrading to non-blocking semantics.
     */
    private static final ThreadLocal<Integer> CALLBACK_DEPTH =
        ThreadLocal.withInitial(() -> 0);

    static boolean inCallback() {
        return CALLBACK_DEPTH.get() > 0;
    }

    static void enterCallback() {
        CALLBACK_DEPTH.set(CALLBACK_DEPTH.get() + 1);
    }

    static void leaveCallback() {
        CALLBACK_DEPTH.set(CALLBACK_DEPTH.get() - 1);
    }

    private final NativeSocketRuntime socket;
    private Arena sendScratchArena = Arena.ofShared();
    private MemorySegment sendScratch = MemorySegment.NULL;
    private int sendScratchCapacity = NativeSocketRuntime.DEFAULT_IO_BUFFER_SIZE;
    private final CompletionOwner completionOwner;
    private final CompletionDispatcher.CompletionLane completionLane;
    SocketCore(NativeSocketRuntime socket,
               CompletionDispatcher.CompletionLane completionLane,
               MemorySegment contextHandle) {
        this.socket = socket;
        this.completionLane = completionLane;
        SocketType type = socket.socketTypeHint();
        this.completionOwner = type == SocketType.PAIR
            || type == SocketType.DEALER
            || type == SocketType.ROUTER
            || type == SocketType.STREAM
            ? new CompletionOwner(socket, completionLane, contextHandle) : null;
    }

    void bind(String endpoint) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment addr = arena.allocateFrom(endpoint, StandardCharsets.UTF_8);
            int rc = Native.bind(socket.handle(), addr);
            if (rc != 0)
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.BIND);
        }
    }

    void connect(String endpoint) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment addr = arena.allocateFrom(endpoint, StandardCharsets.UTF_8);
            int rc = Native.connect(socket.handle(), addr);
            if (rc != 0)
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONNECT);
        }
    }

    void unbind(String endpoint) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment addr = arena.allocateFrom(endpoint, StandardCharsets.UTF_8);
            int rc = Native.unbind(socket.handle(), addr);
            if (rc != 0)
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONNECT);
        }
    }

    void disconnect(String endpoint) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment addr = arena.allocateFrom(endpoint, StandardCharsets.UTF_8);
            int rc = Native.disconnect(socket.handle(), addr);
            if (rc != 0)
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONNECT);
        }
    }

    void disconnectRid(RoutingId peerRid) {
        Objects.requireNonNull(peerRid, "peerRid");
        try (Arena arena = Arena.ofConfined()) {
            byte[] value = InternalAccess.routingIdTrustedBytes(peerRid);
            MemorySegment nativeRid = arena.allocate(NativeLayouts.ROUTING_ID_LAYOUT);
            nativeRid.set(ValueLayout.JAVA_BYTE, NativeLayouts.ROUTING_ID_SIZE_OFFSET,
                (byte) value.length);
            if (value.length > 0) {
                MemorySegment.copy(MemorySegment.ofArray(value), 0, nativeRid,
                    NativeLayouts.ROUTING_ID_DATA_OFFSET, value.length);
            }
            int rc = Native.disconnectRid(socket.handle(), nativeRid);
            if (rc != 0)
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONNECT);
        }
    }

    void setTlsServer(String certPem, String keyPem, boolean requireClientCert) {
        socket.setOption(SocketOptions.TLS_CERT, Objects.requireNonNull(certPem, "certPem"));
        socket.setOption(SocketOptions.TLS_KEY, Objects.requireNonNull(keyPem, "keyPem"));
        socket.setOption(SocketOptions.TLS_REQUIRE_CLIENT_CERT,
            requireClientCert ? 1 : 0);
    }

    void setTlsClient(String caCertPem, String hostname, boolean trustSystem) {
        socket.setOption(SocketOptions.TLS_CA, Objects.requireNonNull(caCertPem, "caCertPem"));
        socket.setOption(SocketOptions.TLS_HOSTNAME, Objects.requireNonNull(hostname, "hostname"));
        socket.setOption(SocketOptions.TLS_TRUST_SYSTEM, trustSystem ? 1 : 0);
    }

    void setSockOpt(int optionId, String optionName, byte[] value) {
        socket.validateOptionAccess(optionId, optionName);
        setSockOpt(optionId, optionName, value, 0, value.length);
    }

    void setSockOpt(int optionId, String optionName, byte[] value,
                    int offset, int length) {
        socket.validateOptionAccess(optionId, optionName);
        Objects.requireNonNull(value, "value");
        NativeSocketRuntime.validateRange(value.length, offset, length, "value");
        socket.setSockOptBytes(optionId, value, offset, length);
    }

    void setSockOpt(int optionId, String optionName, ByteBuffer value) {
        socket.validateOptionAccess(optionId, optionName);
        Objects.requireNonNull(value, "value");
        int length = value.remaining();
        if (length == 0) {
            socket.setSockOptRaw(optionId, MemorySegment.NULL, 0);
            return;
        }
        MemorySegment srcSeg = MemorySegment.ofBuffer(value);
        MemorySegment seg;
        if (value.isDirect()) {
            seg = srcSeg;
        } else {
            seg = socket.ensureSendScratch(length);
            MemorySegment.copy(srcSeg, 0, seg, 0, length);
        }
        socket.setSockOptRaw(optionId, seg, length);
        value.position(value.position() + length);
    }

    void setSockOpt(int optionId, String optionName, int value) {
        socket.validateOptionAccess(optionId, optionName);
        socket.setSockOptInt(optionId, value);
    }

    byte[] getSockOptBytes(int optionId, String optionName, int maxLen) {
        socket.validateOptionAccess(optionId, optionName);
        return socket.getSockOptBytes(optionId, maxLen);
    }

    int getSockOptInt(int optionId, String optionName) {
        socket.validateOptionAccess(optionId, optionName);
        return socket.getSockOptInt(optionId);
    }

    void setOption(SocketOptionKey<Integer> option, int value) {
        Objects.requireNonNull(option, "option");
        socket.validateAmbiguousOption(option);
        socket.validateOptionAccess(option.optionId(), option.name());
        NativeSocketRuntime.validateOptionType(option, SocketOptionValueType.INT32);
        option.requireWritable();
        socket.setSockOptInt(option.optionId(), value);
    }

    void setOptionLong(SocketOptionKey<Long> option, long value) {
        Objects.requireNonNull(option, "option");
        socket.validateAmbiguousOption(option);
        socket.validateOptionAccess(option.optionId(), option.name());
        option.requireLongValue(value);
        option.requireWritable();
        socket.setSockOptLong(option.optionId(), value);
    }

    void setOptionString(SocketOptionKey<String> option, String value) {
        Objects.requireNonNull(option, "option");
        socket.validateAmbiguousOption(option);
        socket.validateOptionAccess(option.optionId(), option.name());
        NativeSocketRuntime.validateOptionType(option, SocketOptionValueType.STRING);
        option.requireWritable();
        byte[] utf8 = Objects.requireNonNull(value, "value").getBytes(
            StandardCharsets.UTF_8);
        socket.setTypedBytesOption(option.optionId(), utf8, 0, utf8.length);
    }

    void setOptionBytes(SocketOptionKey<byte[]> option, byte[] value) {
        Objects.requireNonNull(option, "option");
        socket.validateAmbiguousOption(option);
        socket.validateOptionAccess(option.optionId(), option.name());
        NativeSocketRuntime.validateOptionType(option, SocketOptionValueType.BYTES);
        option.requireWritable();
        Objects.requireNonNull(value, "value");
        socket.setTypedBytesOption(option.optionId(), value, 0, value.length);
    }

    <T> T getOption(SocketOptionKey<T> option) {
        Objects.requireNonNull(option, "option");
        socket.validateAmbiguousOption(option);
        socket.validateOptionAccess(option.optionId(), option.name());
        option.requireReadable();
        return socket.readOption(option);
    }

    SocketMonitor monitorOpen(int events, long monitorHwmBytes) {
        MemorySegment sock = Native.monitorOpen(socket.handle(), events,
            monitorHwmBytes);
        if (sock == null || sock.address() == 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        return InternalAccess.monitorSocket(sock, true);
    }

    void close() {
        socket.closeInternal();
    }

    int closeNativeHandle() {
        return completionOwner == null
            ? Native.close(socket.handle())
            : completionOwner.closeNativeSocket();
    }

    MemorySegment ensureSendScratch(int length) {
        if (length <= 0)
            return MemorySegment.NULL;
        if (sendScratch.address() == 0 || sendScratchCapacity < length) {
            RuntimeResources.closeArena(sendScratchArena);
            sendScratchArena = Arena.ofShared();
            sendScratch = sendScratchArena.allocate(length);
            sendScratchCapacity = length;
        }
        return sendScratch.asSlice(0, length);
    }

    void ensureOpen() {
        if (socket.handle() == null || socket.handle().address() == 0)
            throw new IllegalStateException("socket is closed");
    }

    void dispatchCompletion(Runnable completion) {
        if (completionOwner == null) {
            throw new IllegalStateException(
                "socket does not support completion dispatch");
        }
        completionLane.dispatch(completion);
    }

    CompletionOwner completionOwner() {
        if (completionOwner == null) {
            throw new ZlinkSubmitException(SubmitResult.NOT_SUPPORTED,
                NativeErrno.ENOTSUP);
        }
        return completionOwner;
    }

    void closeCommonState() {
        if (completionOwner != null)
            completionOwner.close();
        RuntimeResources.closeArena(sendScratchArena);
        sendScratchArena = null;
        sendScratch = MemorySegment.NULL;
    }

}
