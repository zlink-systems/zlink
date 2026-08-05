/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;
import systems.zlink.internal.sockets.SocketOptionKey;
import systems.zlink.internal.sockets.SocketOptions;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.charset.StandardCharsets;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeIntOptions;
import systems.zlink.runtime.nativeapi.NativeLayouts;

final class SocketOptionSupport {
    private static final int OPT_RCVMORE = 13;

    private final NativeSocketRuntime socket;

    SocketOptionSupport(NativeSocketRuntime socket) {
        this.socket = socket;
    }

    void setDealerIntOption(int option, int value) {
        setNativeIntOption(option, value, Native::setDealerOption);
    }

    int getDealerIntOption(int option) {
        return getNativeIntOption(option, Native::getDealerOption);
    }

    int getRouterIntOption(int option) {
        return getNativeIntOption(option, Native::getRouterOption);
    }

    void setRouterIntOption(int option, int value) {
        setNativeIntOption(option, value, Native::setRouterOption);
    }

    @SuppressWarnings("unchecked")
    <T> T readOption(SocketOptionKey<T> option) {
        return switch (option.valueType()) {
            case INT32 -> (T) Integer.valueOf(getSockOptInt(option.optionId()));
            case INT64, UINT64 ->
                (T) Long.valueOf(getSockOptLong(option.optionId()));
            case STRING -> (T) getTypedStringOption(option);
            case BYTES -> (T) getTypedBytesOption(option);
        };
    }

    void setSockOptRaw(int optionId, MemorySegment value, long len) {
        var route = optionRoute(optionId);
        int rc = switch (route.family()) {
            case ROUTER -> Native.setRouterOption(socket.handle(),
                route.optionId(), value, len);
            case PUB -> Native.setPubOption(socket.handle(), route.optionId(),
                value, len);
            case SUB -> Native.setSubOption(socket.handle(), route.optionId(),
                value, len);
            case STREAM -> Native.setStreamOption(socket.handle(),
                route.optionId(), value, len);
            case COMMON -> Native.setSockOpt(socket.handle(),
                route.nativeCommonOptionId(), value, len);
        };
        if (rc != 0) {
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
    }

    void setSockOptBytes(int optionId, byte[] value, int offset, int length) {
        MemorySegment buf = length == 0 ? MemorySegment.NULL
            : socket.ensureSendScratch(length);
        if (length > 0) {
            MemorySegment.copy(MemorySegment.ofArray(value), offset, buf, 0,
                length);
        }
        setSockOptRaw(optionId, buf, length);
    }

    void setTypedBytesOption(int optionId, byte[] value, int offset,
                             int length) {
        if (optionId == SocketOptions.ROUTING_ID.optionId()) {
            setRoutingIdBytes(value, offset, length);
            return;
        }
        if (optionId == 6) {
            setSubscriptionBytes(value, offset, length, true);
            return;
        }
        if (optionId == 7) {
            setSubscriptionBytes(value, offset, length, false);
            return;
        }
        setSockOptBytes(optionId, value, offset, length);
    }

    void setSockOptInt(int optionId, int value) {
        MemorySegment buf = socket.ensureSendScratch(Integer.BYTES);
        buf.set(ValueLayout.JAVA_INT, 0, value);
        setSockOptRaw(optionId, buf, Integer.BYTES);
    }

    void setSockOptLong(int optionId, long value) {
        MemorySegment buf = socket.ensureSendScratch(Long.BYTES);
        buf.set(ValueLayout.JAVA_LONG, 0, value);
        setSockOptRaw(optionId, buf, Long.BYTES);
    }

    byte[] getSockOptBytes(int optionId, int maxLen) {
        if (maxLen < 0) {
            throw new IllegalArgumentException("maxLen must be >= 0");
        }
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment buf = maxLen == 0 ? MemorySegment.NULL
                : arena.allocate(maxLen);
            MemorySegment len = arena.allocate(ValueLayout.JAVA_LONG);
            len.set(ValueLayout.JAVA_LONG, 0, maxLen);
            var route = optionRoute(optionId);
            int rc = dispatchGet(route, buf, len);
            if (rc != 0) {
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
            long actualLong = len.get(ValueLayout.JAVA_LONG, 0);
            if (actualLong < 0) {
                actualLong = 0;
            }
            if (actualLong > maxLen) {
                actualLong = maxLen;
            }
            int actual = (int) actualLong;
            if (actual == 0) {
                return new byte[0];
            }
            byte[] out = new byte[actual];
            MemorySegment.copy(buf, 0, MemorySegment.ofArray(out), 0, actual);
            return out;
        }
    }

    int getSockOptInt(int optionId) {
        if (optionId == OPT_RCVMORE) {
            return socket.pendingFrameCount() > 0 ? 1 : 0;
        }
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment buf = arena.allocate(ValueLayout.JAVA_INT);
            MemorySegment len = arena.allocate(ValueLayout.JAVA_LONG);
            len.set(ValueLayout.JAVA_LONG, 0, ValueLayout.JAVA_INT.byteSize());
            var route = optionRoute(optionId);
            int rc = dispatchGet(route, buf, len);
            if (rc != 0) {
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
            return buf.get(ValueLayout.JAVA_INT, 0);
        }
    }

    private int getNativeIntOption(int option, NativeIntOptionGetter getter) {
        socket.ensureOpen();
        return NativeIntOptions.get(socket.handle(), option, getter::get);
    }

    private void setNativeIntOption(int option, int value,
                                    NativeIntOptionSetter setter) {
        socket.ensureOpen();
        NativeIntOptions.set(socket.handle(), option, value, setter::set);
    }

    @SuppressWarnings("unchecked")
    private <T> T getTypedStringOption(SocketOptionKey<T> option) {
        if (option.optionId() == SocketOptions.ROUTING_ID.optionId()) {
            return (T) new String(getRoutingIdBytes(), StandardCharsets.UTF_8);
        }
        return (T) decodeCString(
            getSockOptBytes(option.optionId(), option.maxReadLength()));
    }

    @SuppressWarnings("unchecked")
    private <T> T getTypedBytesOption(SocketOptionKey<T> option) {
        if (option.optionId() == SocketOptions.ROUTING_ID.optionId()) {
            return (T) getRoutingIdBytes();
        }
        return (T) getSockOptBytes(option.optionId(), option.maxReadLength());
    }

    void setRoutingIdBytes(byte[] value, int offset, int length) {
        MemorySegment buf = length == 0 ? MemorySegment.NULL
            : socket.ensureSendScratch(length);
        if (length > 0) {
            MemorySegment.copy(MemorySegment.ofArray(value), offset, buf, 0,
                length);
        }
        int rc = Native.setRoutingId(socket.handle(), buf, length);
        if (rc != 0) {
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
    }

    byte[] getRoutingIdBytes() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment outRid = arena.allocate(
                NativeLayouts.ROUTING_ID_LAYOUT);
            int rc = Native.getRoutingId(socket.handle(), outRid);
            if (rc != 0) {
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
            int size = outRid.get(ValueLayout.JAVA_BYTE,
                NativeLayouts.ROUTING_ID_SIZE_OFFSET) & 0xFF;
            byte[] out = new byte[size];
            if (size > 0) {
                MemorySegment.copy(outRid, NativeLayouts.ROUTING_ID_DATA_OFFSET,
                    MemorySegment.ofArray(out), 0, size);
            }
            return out;
        }
    }

    void setSubscriptionBytes(byte[] value, int offset, int length,
                              boolean subscribe) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment filter = length == 0 ? arena.allocate(1)
                : arena.allocate(length + 1L);
            if (length > 0) {
                MemorySegment.copy(MemorySegment.ofArray(value), offset, filter,
                    0, length);
            }
            filter.set(ValueLayout.JAVA_BYTE, length, (byte) 0);
            int rc = subscribe ? Native.setSubscription(socket.handle(), filter)
                : Native.unsetSubscription(socket.handle(), filter);
            if (rc != 0) {
                throw ZlinkException.fromLastError(
                    systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
        }
    }

    private long getSockOptLong(int optionId) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment buf = arena.allocate(ValueLayout.JAVA_LONG);
            MemorySegment len = arena.allocate(ValueLayout.JAVA_LONG);
            len.set(ValueLayout.JAVA_LONG, 0, ValueLayout.JAVA_LONG.byteSize());
            var route = optionRoute(optionId);
            int rc = dispatchGet(route, buf, len);
            if (rc != 0) {
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
            return buf.get(ValueLayout.JAVA_LONG, 0);
        }
    }

    private SocketOptionRouter.Route optionRoute(int optionId) {
        return SocketOptionRouter.route(optionId, socket.resolveSocketType());
    }

    private int dispatchGet(SocketOptionRouter.Route route, MemorySegment value,
                            MemorySegment len) {
        return switch (route.family()) {
            case ROUTER -> Native.getRouterOption(socket.handle(),
                route.optionId(), value, len);
            case PUB -> Native.getPubOption(socket.handle(), route.optionId(),
                value, len);
            case SUB -> Native.getSubOption(socket.handle(), route.optionId(),
                value, len);
            case STREAM -> Native.getStreamOption(socket.handle(),
                route.optionId(), value, len);
            case COMMON -> Native.getSockOpt(socket.handle(),
                route.nativeCommonOptionId(), value, len);
        };
    }

    private static String decodeCString(byte[] raw) {
        int len = raw.length;
        for (int i = 0; i < raw.length; i++) {
            if (raw[i] == 0) {
                len = i;
                break;
            }
        }
        if (len == 0) {
            return "";
        }
        return new String(raw, 0, len, StandardCharsets.UTF_8);
    }

    @FunctionalInterface
    private interface NativeIntOptionGetter {
        int get(MemorySegment handle, int option, MemorySegment value,
                MemorySegment len);
    }

    @FunctionalInterface
    private interface NativeIntOptionSetter {
        int set(MemorySegment handle, int option, MemorySegment value,
                long len);
    }
}
