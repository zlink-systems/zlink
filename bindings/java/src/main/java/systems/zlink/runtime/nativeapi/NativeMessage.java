package systems.zlink.runtime.nativeapi;

import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.errors.ConfigResult;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

public final class NativeMessage {
    private static final MethodHandle MH_MSG_INIT = NativeSymbols.downcallCritical("zlink_msg_init",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_MSG_INIT_SIZE = NativeSymbols.downcallCritical("zlink_msg_init_size",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_MSG_CLOSE = NativeSymbols.downcallCritical("zlink_msg_close",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_MSG_MOVE = NativeSymbols.downcallCritical("zlink_msg_move",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_MSG_COPY = NativeSymbols.downcallCritical("zlink_msg_copy",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_MSG_DATA = NativeSymbols.downcallCritical("zlink_msg_data",
            FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_MSG_DATA_ADDR = NativeSymbols.downcallCritical(
            "zlink_java_msg_data_addr",
            FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));
    private static final MethodHandle MH_MSG_SIZE = NativeSymbols.downcallCritical("zlink_msg_size",
            FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));
    private static final MethodHandle MH_MSG_REFCNT = NativeSymbols.downcallCritical("zlink_msg_refcnt",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_MSGV_CLOSE = NativeSymbols.downcallAny(
            new String[] {"zlink_multipart_close", "zlink_msgv_close"},
            FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_FREE = NativeSymbols.freeDowncall();
    // Callback-based receive attaches a native msg handler. The Core raw API supports
    // this only for raw STREAM subjects; other subjects fail with errno=ENOTSUP.
    private static final MethodHandle MH_RECV_HANDLER = NativeSymbols.downcall(
            "zlink_recv_handler",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS));

    private NativeMessage() {}

    public static int messageInit(MemorySegment msg) {
        try {
            return (int) MH_MSG_INIT.invokeExact(msg);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_msg_init failed", t);
        }
    }

    public static int messageInitSize(MemorySegment msg, int size) {
        try {
            return (int) MH_MSG_INIT_SIZE.invokeExact(msg, (long) size);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_msg_init_size failed", t);
        }
    }

    public static int messageClose(MemorySegment msg) {
        try {
            return (int) MH_MSG_CLOSE.invokeExact(msg);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_msg_close failed", t);
        }
    }

    public static int messageMove(MemorySegment dest, MemorySegment src) {
        try {
            return (int) MH_MSG_MOVE.invokeExact(dest, src);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_msg_move failed", t);
        }
    }

    public static int messageCopy(MemorySegment dest, MemorySegment src) {
        try {
            return (int) MH_MSG_COPY.invokeExact(dest, src);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_msg_copy failed", t);
        }
    }

    public static MemorySegment messageData(MemorySegment msg) {
        try {
            return (MemorySegment) MH_MSG_DATA.invokeExact(msg);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_msg_data failed", t);
        }
    }

    public static long messageDataAddress(MemorySegment msg) {
        try {
            return (long) MH_MSG_DATA_ADDR.invokeExact(msg);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_java_msg_data_addr failed", t);
        }
    }

    public static long messageSize(MemorySegment msg) {
        try {
            return (long) MH_MSG_SIZE.invokeExact(msg);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_msg_size failed", t);
        }
    }

    public static int messageRefCount(MemorySegment msg) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment errorOut = arena.allocate(ValueLayout.JAVA_INT);
            int refCount = (int) MH_MSG_REFCNT.invokeExact(msg, errorOut);
            int configResult = errorOut.get(ValueLayout.JAVA_INT, 0);
            if (refCount < 0 || configResult != 0) {
                throw new ZlinkConfigException(ConfigResult.fromValue(configResult));
            }
            return refCount;
        } catch (ZlinkConfigException ex) {
            throw ex;
        } catch (Throwable t) {
            throw new RuntimeException("zlink_msg_refcnt failed", t);
        }
    }

    public static void messageVectorClose(MemorySegment parts, long count) {
        try {
            MH_MSGV_CLOSE.invokeExact(parts, count);
            if (parts != null && parts.address() != 0) {
                MH_FREE.invokeExact(parts);
            }
        } catch (Throwable t) {
            throw new RuntimeException("zlink_multipart_close failed", t);
        }
    }

    public static void multipartClose(MemorySegment parts, long count) {
        try {
            MH_MSGV_CLOSE.invokeExact(parts, count);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_multipart_close failed", t);
        }
    }

    public static int recvHandler(MemorySegment socket,
                                  MemorySegment handler,
                                  MemorySegment userData) {
        try {
            return (int) MH_RECV_HANDLER.invokeExact(socket, handler,
                userData);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_recv_handler failed", t);
        }
    }

}
