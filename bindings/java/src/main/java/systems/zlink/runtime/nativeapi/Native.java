package systems.zlink.runtime.nativeapi;

import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.errors.ZlinkException;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.util.ArrayList;
import java.util.List;

public final class Native {
    public static final int PART_FINAL = 0;
    public static final int PART_MORE = 1;
    private static final ThreadLocal<NativeMultipartScratch>
        MULTIPART_RECEIVE_SCRATCH =
            ThreadLocal.withInitial(NativeMultipartScratch::new);

    private static MethodHandle downcall(String name, FunctionDescriptor fd) {
        return NativeSymbols.downcall(name, fd);
    }

    private static MethodHandle downcallCritical(String name,
                                                 FunctionDescriptor fd) {
        return NativeSymbols.downcallCritical(name, fd);
    }


    private static final MethodHandle MH_VERSION = downcall("zlink_version",
            FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_CTX_NEW = downcall("zlink_ctx_new",
            FunctionDescriptor.of(ValueLayout.ADDRESS));
    private static final MethodHandle MH_CTX_TERM = downcall("zlink_ctx_term",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_CTX_SET = downcall("zlink_ctx_set",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_CTX_SET_DATA = downcall("zlink_ctx_set_data",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_CTX_GET = downcall("zlink_ctx_get",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_CTX_GET_DATA = downcall(
            "zlink_ctx_get_data",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_CTX_SHUTDOWN = downcall("zlink_ctx_shutdown",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_CTX_AUTO_HWM_RECALCULATE = downcall(
            "zlink_ctx_auto_hwm_recalculate",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_SOCKET = downcall("zlink_socket",
            FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_CLOSE = downcall("zlink_close",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_BIND = downcall("zlink_bind",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_CONNECT = downcall("zlink_connect",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_UNBIND = downcall("zlink_unbind",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_DISCONNECT = downcall("zlink_disconnect",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_DISCONNECT_RID = downcall("zlink_disconnect_rid",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_RECV_HANDLER = downcall(
            "zlink_recv_handler",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_SEND_READY_HANDLER = downcall(
            "zlink_send_ready_handler",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_SEND_PART = downcall("zlink_send_part",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                    ValueLayout.JAVA_INT));
    // DONT_WAIT-only critical variant. zlink_send_part is non-blocking when
    // called with DONT_WAIT, so the JVM can elide GC safepoint transitions.
    private static final MethodHandle MH_SEND_PART_CRITICAL =
            downcallCritical("zlink_send_part",
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_SEND_PART_RID = downcall(
            "zlink_send_part_rid",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));
    // DONT_WAIT-only critical variant for routed send.
    private static final MethodHandle MH_SEND_PART_RID_CRITICAL =
            downcallCritical("zlink_send_part_rid",
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.JAVA_INT));
    private static final MethodHandle MH_JAVA_SEND_U32 = downcall(
            "zlink_java_send_u32",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_RECV_PART = downcall("zlink_recv_part",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    // DONT_WAIT-only critical variant. Caller MUST guarantee DONT_WAIT so
    // zlink_recv_part cannot block while the JVM elides safepoint transitions.
    private static final MethodHandle MH_RECV_PART_CRITICAL =
            downcallCritical("zlink_recv_part",
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.JAVA_INT));
    private static final MethodHandle MH_STREAM_PACKET_HANDLER = downcall(
            "zlink_stream_packet_handler",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_SETSOCKOPT = downcall("zlink_set_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_GETSOCKOPT = downcall("zlink_get_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_SET_ROUTER_OPTION = downcall(
            "zlink_set_router_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_GET_ROUTER_OPTION = downcall(
            "zlink_get_router_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_SET_DEALER_OPTION = downcall(
            "zlink_set_dealer_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_GET_DEALER_OPTION = downcall(
            "zlink_get_dealer_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_SET_PUB_OPTION = downcall(
            "zlink_set_pub_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_GET_PUB_OPTION = downcall(
            "zlink_get_pub_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_SET_SUB_OPTION = downcall(
            "zlink_set_sub_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_GET_SUB_OPTION = downcall(
            "zlink_get_sub_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_SET_STREAM_OPTION = downcall(
            "zlink_set_stream_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_GET_STREAM_OPTION = downcall(
            "zlink_get_stream_option",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_SET_ROUTING_ID = downcall("zlink_set_routing_id",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_GET_ROUTING_ID = downcall("zlink_get_routing_id",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_SET_SUBSCRIPTION = downcall("zlink_set_subscription",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_UNSET_SUBSCRIPTION = downcall("zlink_unset_subscription",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_SUBSCRIPTION_AT = downcall("zlink_subscription_at",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_PUBLISH_PART = downcall(
            "zlink_publish_part",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));
    // DONT_WAIT-only critical variant. zlink_publish_part is non-blocking when
    // called with DONT_WAIT, so the JVM can elide GC safepoint transitions on
    // the publish hot path (parity with MH_SEND_PART_CRITICAL).
    private static final MethodHandle MH_PUBLISH_PART_CRITICAL =
            downcallCritical("zlink_publish_part",
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.JAVA_INT));
    private static final MethodHandle MH_SUBSCRIBE_PART = downcall(
            "zlink_subscribe_part",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT));
    // DONT_WAIT-only critical variant. zlink_subscribe_part is non-blocking
    // when called with DONT_WAIT, so the JVM can elide GC safepoint
    // transitions on the subscribe hot path (parity with
    // MH_RECV_PART_CRITICAL).
    private static final MethodHandle MH_SUBSCRIBE_PART_CRITICAL =
            downcallCritical("zlink_subscribe_part",
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_LONG,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_XPUB_RECV_PART = downcall(
            "zlink_xpub_recv_part",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_LONG,
                    ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT));

    private static final MethodHandle MH_MONITOR_OPEN = downcall("zlink_socket_monitor_open",
            FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_MONITOR_HANDLER = downcall(
      "zlink_socket_monitor_handler",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_MONITOR_RECV = downcall("zlink_socket_monitor_recv",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_MONITOR_SNAPSHOT = downcall("zlink_monitor_status",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_MONITOR_CLOSE = downcall("zlink_monitor_close",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_ERRNO = downcallCritical("zlink_errno",
            FunctionDescriptor.of(ValueLayout.JAVA_INT));
    private static final MethodHandle MH_STRERROR = downcall("zlink_strerror",
            FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_HAS = downcall("zlink_has",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_SLEEP = downcall("zlink_sleep",
            FunctionDescriptor.ofVoid(ValueLayout.JAVA_INT));
    private static final MethodHandle MH_SET_TLS_SRV = downcall(
      "zlink_set_tls_server",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_SET_TLS_CLI = downcall(
      "zlink_set_tls_client",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_PROXY = downcall("zlink_proxy",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_PROXY_STEERABLE = downcall(
      "zlink_proxy_steerable",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS));

    private static final MethodHandle MH_ATOMIC_COUNTER_NEW = downcall(
      "zlink_atomic_counter_new",
      FunctionDescriptor.of(ValueLayout.ADDRESS));
    private static final MethodHandle MH_ATOMIC_COUNTER_SET = downcall(
      "zlink_atomic_counter_set",
      FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_ATOMIC_COUNTER_INC = downcall(
      "zlink_atomic_counter_inc",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_ATOMIC_COUNTER_DEC = downcall(
      "zlink_atomic_counter_dec",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_ATOMIC_COUNTER_VALUE = downcall(
      "zlink_atomic_counter_value",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_ATOMIC_COUNTER_DESTROY = downcall(
      "zlink_atomic_counter_destroy",
      FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

    private static final MethodHandle MH_TIMER_NEW = downcall("zlink_timer_new",
      FunctionDescriptor.of(ValueLayout.ADDRESS));
    private static final MethodHandle MH_TIMER_DESTROY = downcall(
      "zlink_timer_destroy",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_TIMER_START = downcall(
      "zlink_timer_start",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_TIMER_STOP = downcall(
      "zlink_timer_stop",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_TIMER_RECV = downcall(
      "zlink_timer_recv",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS));
    private static final MethodHandle MH_TIMER_HANDLER = downcall(
      "zlink_timer_handler",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS));

    private static final MethodHandle MH_STOPWATCH_START = downcall(
      "zlink_stopwatch_start",
      FunctionDescriptor.of(ValueLayout.ADDRESS));
    private static final MethodHandle MH_STOPWATCH_INTERMEDIATE = downcall(
      "zlink_stopwatch_intermediate",
      FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));
    private static final MethodHandle MH_STOPWATCH_STOP = downcall(
      "zlink_stopwatch_stop",
      FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));

    private static final MethodHandle MH_THREAD_START = downcall(
      "zlink_thread_start",
      FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS));
    private static final MethodHandle MH_THREAD_JOIN = downcall(
      "zlink_thread_join",
      FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

    private static final MethodHandle MH_ROUTER_RECV_PART = downcall(
      "zlink_router_recv_part",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    // DONT_WAIT-only critical variant. zlink_router_recv_part is non-blocking
    // when called with DONT_WAIT flag, so the JVM can elide GC safepoint
    // transition for this call. Caller must guarantee DONT_WAIT bit is set.
    private static final MethodHandle MH_ROUTER_RECV_PART_CRITICAL =
      downcallCritical(
        "zlink_router_recv_part",
        FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
          ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
          ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_ROUTER_REQUEST_PART = downcall(
      "zlink_router_request_part",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
        ValueLayout.JAVA_INT, ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS));
    private static final MethodHandle MH_DEALER_REQUEST_PART = downcall(
      "zlink_dealer_request_part",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.JAVA_INT, ValueLayout.JAVA_INT,
        ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_ROUTER_REPLY_PART = downcall(
      "zlink_router_reply_part",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.ADDRESS,
        ValueLayout.JAVA_INT));
    private static final MethodHandle MH_ROUTER_COMPLETION_CONTROL_PART = downcall(
      "zlink_router_completion_control_part",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_ROUTER_COMPLETION_CONTROL_HANDLER = downcall(
      "zlink_router_completion_control_handler",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS));


    private Native() {}

    private static boolean invalidMultipart(MemorySegment parts,
                                            long partCount) {
        return partCount <= 0
            || parts == null
            || parts.address() == 0;
    }

    static MemorySegment nthPart(MemorySegment parts, long index) {
        long messageSize = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        return parts.asSlice(index * messageSize, messageSize);
    }

    private static int sendMultipartLoop(MemorySegment socket,
                                         MemorySegment routingId,
                                         MemorySegment parts,
                                         long partCount,
                                         int flags) {
        if (invalidMultipart(parts, partCount)) {
            return SubmitResult.INVALID_ARGUMENT.value();
        }
        for (long i = 0; i < partCount; i++) {
            int partFlag = i + 1 < partCount ? PART_MORE : PART_FINAL;
            int rc = routingId == null || routingId.address() == 0
                ? sendPart(socket, nthPart(parts, i), flags, partFlag)
                : sendPartRid(socket, routingId, nthPart(parts, i), flags,
                    partFlag);
            if (rc != 0) {
                return rc;
            }
        }
        return 0;
    }

    private static void copyRoutingIdOut(MemorySegment target,
                                         MemorySegment routingIdPtr) {
        if (target == null || target.address() == 0) {
            return;
        }
        NativeRoutingIds.copyTo(target, routingIdPtr);
    }

    public static int[] version() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment major = arena.allocate(ValueLayout.JAVA_INT);
            MemorySegment minor = arena.allocate(ValueLayout.JAVA_INT);
            MemorySegment patch = arena.allocate(ValueLayout.JAVA_INT);
            MH_VERSION.invokeExact(major, minor, patch);
            return new int[] {
                    major.get(ValueLayout.JAVA_INT, 0),
                    minor.get(ValueLayout.JAVA_INT, 0),
                    patch.get(ValueLayout.JAVA_INT, 0)
            };
        } catch (Throwable t) {
            throw new RuntimeException("zlink_version failed", t);
        }
    }

    public static MemorySegment ctxNew() {
        try {
            return (MemorySegment) MH_CTX_NEW.invokeExact();
        } catch (Throwable t) {
            throw new RuntimeException("zlink_ctx_new failed", t);
        }
    }

    public static int ctxTerm(MemorySegment ctx) {
        try {
            return (int) MH_CTX_TERM.invokeExact(ctx);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_ctx_term failed", t);
        }
    }

    public static int ctxSet(MemorySegment ctx, int option, int value) {
        try {
            return (int) MH_CTX_SET.invokeExact(ctx, option, value);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_ctx_set failed", t);
        }
    }

    public static int ctxSetData(MemorySegment ctx, int option,
                                 MemorySegment value, long valueLength) {
        try {
            return (int) MH_CTX_SET_DATA.invokeExact(ctx, option, value,
              valueLength);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_ctx_set_data failed", t);
        }
    }

    public static int ctxGet(MemorySegment ctx, int option) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment errorOut = arena.allocate(ValueLayout.JAVA_INT);
            return (int) MH_CTX_GET.invokeExact(ctx, option, errorOut);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_ctx_get failed", t);
        }
    }

    public static int ctxGetData(MemorySegment ctx, int option,
                                 MemorySegment value,
                                 MemorySegment valueLength) {
        try {
            return (int) MH_CTX_GET_DATA.invokeExact(ctx, option, value,
              valueLength);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_ctx_get_data failed", t);
        }
    }

    public static int ctxShutdown(MemorySegment ctx) {
        try {
            return (int) MH_CTX_SHUTDOWN.invokeExact(ctx);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_ctx_shutdown failed", t);
        }
    }

    public static int ctxAutoHwmRecalculate(MemorySegment ctx) {
        try {
            return (int) MH_CTX_AUTO_HWM_RECALCULATE.invokeExact(ctx);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_ctx_auto_hwm_recalculate failed", t);
        }
    }

    public static MemorySegment socket(MemorySegment ctx, int type) {
        try {
            return (MemorySegment) MH_SOCKET.invokeExact(ctx, type);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_socket failed", t);
        }
    }

    public static int close(MemorySegment socket) {
        try {
            return (int) MH_CLOSE.invokeExact(socket);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_close failed", t);
        }
    }

    public static int bind(MemorySegment socket, MemorySegment addr) {
        try {
            return (int) MH_BIND.invokeExact(socket, addr);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_bind failed", t);
        }
    }

    public static int connect(MemorySegment socket, MemorySegment addr) {
        try {
            return (int) MH_CONNECT.invokeExact(socket, addr);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_connect failed", t);
        }
    }

    public static int unbind(MemorySegment socket, MemorySegment addr) {
        try {
            return (int) MH_UNBIND.invokeExact(socket, addr);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_unbind failed", t);
        }
    }

    public static int disconnect(MemorySegment socket, MemorySegment addr) {
        try {
            return (int) MH_DISCONNECT.invokeExact(socket, addr);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_disconnect failed", t);
        }
    }

    public static int disconnectRid(MemorySegment socket, MemorySegment peerRid) {
        try {
            return (int) MH_DISCONNECT_RID.invokeExact(socket, peerRid);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_disconnect_rid failed", t);
        }
    }

    public static int recvHandler(MemorySegment handle, MemorySegment handler,
                                  MemorySegment userdata) {
        try {
            return (int) MH_RECV_HANDLER.invokeExact(handle, handler, userdata);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_recv_handler failed", t);
        }
    }

    public static int sendReadyHandler(MemorySegment handle,
                                       MemorySegment handler,
                                       MemorySegment userdata) {
        try {
            return (int) MH_SEND_READY_HANDLER.invokeExact(handle, handler,
                userdata);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_send_ready_handler failed", t);
        }
    }

    public static int sendMultipart(MemorySegment socket, MemorySegment parts,
                                    long partCount, int flags) {
        try {
            return sendMultipartLoop(socket, MemorySegment.NULL, parts,
                partCount, flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_send_part failed", t);
        }
    }

    public static int sendPart(MemorySegment socket, MemorySegment part,
                               int flags, int partFlag) {
        try {
            return (int) MH_SEND_PART.invokeExact(socket, part, flags, partFlag);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_send_part failed", t);
        }
    }

    // DONT_WAIT-only critical variant. Caller MUST guarantee DONT_WAIT bit set.
    public static int sendPartNoWaitCritical(MemorySegment socket,
                                             MemorySegment part,
                                             int flags, int partFlag) {
        try {
            return (int) MH_SEND_PART_CRITICAL.invokeExact(socket, part, flags,
                partFlag);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_send_part (critical) failed", t);
        }
    }

    public static int sendMultipart(MemorySegment socket, MemorySegment routingId,
                                    MemorySegment parts, long partCount,
                                    int flags) {
        try {
            return sendMultipartLoop(socket, routingId, parts, partCount,
                flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_send_part_rid failed", t);
        }
    }

    public static int sendPartRid(MemorySegment socket,
                                  MemorySegment routingId,
                                  MemorySegment part,
                                  int flags,
                                  int partFlag) {
        try {
            return (int) MH_SEND_PART_RID.invokeExact(socket, routingId, part,
                flags, partFlag);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_send_part_rid failed", t);
        }
    }

    // DONT_WAIT-only critical variant for routed send.
    public static int sendPartRidNoWaitCritical(MemorySegment socket,
                                                MemorySegment routingId,
                                                MemorySegment part,
                                                int flags,
                                                int partFlag) {
        try {
            return (int) MH_SEND_PART_RID_CRITICAL.invokeExact(socket,
                routingId, part, flags, partFlag);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_send_part_rid (critical) failed",
                t);
        }
    }

    public static int sendMultipartU32(MemorySegment socket, int routingId,
                                       MemorySegment parts, long partCount,
                                       int flags) {
        try {
            return (int) MH_JAVA_SEND_U32.invokeExact(socket, routingId, parts,
                partCount, flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_java_send_u32 failed", t);
        }
    }

    public static int recv(MemorySegment socket, MemorySegment sourceRidOut,
                               MemorySegment partOut,
                               MemorySegment hasMoreOut,
                               int flags) {
        try {
            return (int) MH_RECV_PART.invokeExact(socket, sourceRidOut, partOut,
                hasMoreOut, flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_recv_part failed", t);
        }
    }

    public static int recvPartNoWaitCritical(MemorySegment socket,
                                             MemorySegment sourceRidOut,
                                             MemorySegment partOut,
                                             MemorySegment hasMoreOut,
                                             int flags) {
        try {
            return (int) MH_RECV_PART_CRITICAL.invokeExact(socket,
                sourceRidOut, partOut, hasMoreOut, flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_recv_part (critical) failed", t);
        }
    }

    public static int streamPacketHandler(MemorySegment socket,
                                          MemorySegment callback) {
        try {
            return (int) MH_STREAM_PACKET_HANDLER.invokeExact(socket, callback,
                MemorySegment.NULL);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_stream_packet_handler failed", t);
        }
    }

    public static int setSockOpt(MemorySegment socket, int option, MemorySegment value, long len) {
        try {
            return (int) MH_SETSOCKOPT.invokeExact(socket, option, value, len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_setsockopt failed", t);
        }
    }

    public static int getSockOpt(MemorySegment socket, int option, MemorySegment value, MemorySegment len) {
        try {
            return (int) MH_GETSOCKOPT.invokeExact(socket, option, value, len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_getsockopt failed", t);
        }
    }

    public static int setRouterOption(MemorySegment handle, int option,
                                      MemorySegment value, long len) {
        try {
            return (int) MH_SET_ROUTER_OPTION.invokeExact(handle, option, value,
              len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_set_router_option failed", t);
        }
    }

    public static int getRouterOption(MemorySegment handle, int option,
                                      MemorySegment value,
                                      MemorySegment len) {
        try {
            return (int) MH_GET_ROUTER_OPTION.invokeExact(handle, option, value,
              len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_get_router_option failed", t);
        }
    }

    public static int setDealerOption(MemorySegment handle, int option,
                                      MemorySegment value, long len) {
        try {
            return (int) MH_SET_DEALER_OPTION.invokeExact(handle, option, value,
              len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_set_dealer_option failed", t);
        }
    }

    public static int getDealerOption(MemorySegment handle, int option,
                                      MemorySegment value, MemorySegment len) {
        try {
            return (int) MH_GET_DEALER_OPTION.invokeExact(handle, option, value,
              len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_get_dealer_option failed", t);
        }
    }

    public static int setPubOption(MemorySegment handle, int option,
                                   MemorySegment value, long len) {
        try {
            return (int) MH_SET_PUB_OPTION.invokeExact(handle, option, value,
              len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_set_pub_option failed", t);
        }
    }

    public static int getPubOption(MemorySegment handle, int option,
                                   MemorySegment value, MemorySegment len) {
        try {
            return (int) MH_GET_PUB_OPTION.invokeExact(handle, option, value,
              len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_get_pub_option failed", t);
        }
    }

    public static int setSubOption(MemorySegment handle, int option,
                                   MemorySegment value, long len) {
        try {
            return (int) MH_SET_SUB_OPTION.invokeExact(handle, option, value,
              len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_set_sub_option failed", t);
        }
    }

    public static int getSubOption(MemorySegment handle, int option,
                                   MemorySegment value, MemorySegment len) {
        try {
            return (int) MH_GET_SUB_OPTION.invokeExact(handle, option, value,
              len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_get_sub_option failed", t);
        }
    }

    public static int setStreamOption(MemorySegment handle, int option,
                                      MemorySegment value, long len) {
        try {
            return (int) MH_SET_STREAM_OPTION.invokeExact(handle, option, value,
              len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_set_stream_option failed", t);
        }
    }

    public static int getStreamOption(MemorySegment handle, int option,
                                      MemorySegment value,
                                      MemorySegment len) {
        try {
            return (int) MH_GET_STREAM_OPTION.invokeExact(handle, option, value,
              len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_get_stream_option failed", t);
        }
    }

    public static int setRoutingId(MemorySegment handle, MemorySegment value,
                                   long len) {
        try {
            return (int) MH_SET_ROUTING_ID.invokeExact(handle, value, len);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_set_routing_id failed", t);
        }
    }

    public static int getRoutingId(MemorySegment handle, MemorySegment outRid) {
        try {
            return (int) MH_GET_ROUTING_ID.invokeExact(handle, outRid);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_get_routing_id failed", t);
        }
    }

    public static int setSubscription(MemorySegment handle,
                                      MemorySegment filter) {
        try {
            return (int) MH_SET_SUBSCRIPTION.invokeExact(handle, filter);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_set_subscription failed", t);
        }
    }

    public static int unsetSubscription(MemorySegment handle,
                                        MemorySegment filter) {
        try {
            return (int) MH_UNSET_SUBSCRIPTION.invokeExact(handle, filter);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_unset_subscription failed", t);
        }
    }

    public static int subscriptionAt(MemorySegment handle, long index,
                                     MemorySegment filterOut,
                                     MemorySegment filterLenInOut,
                                     MemorySegment isPatternOut) {
        try {
            return (int) MH_SUBSCRIPTION_AT.invokeExact(handle, index,
              filterOut, filterLenInOut, isPatternOut);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_subscription_at failed", t);
        }
    }

    public static int publish(MemorySegment subject, MemorySegment topicId,
                              MemorySegment parts, long partCount,
                              int flags) {
        try {
            if (invalidMultipart(parts, partCount)) {
                return SubmitResult.INVALID_ARGUMENT.value();
            }
            for (long i = 0; i < partCount; i++) {
                int partFlag = i + 1 < partCount ? PART_MORE : PART_FINAL;
                int rc = publishPart(subject, topicId,
                    nthPart(parts, i), flags, partFlag);
                if (rc != 0) {
                    return rc;
                }
            }
            return 0;
        } catch (Throwable t) {
            throw new RuntimeException("zlink_publish_part failed", t);
        }
    }

    public static int publishPart(MemorySegment subject, MemorySegment topicId,
                                  MemorySegment part, int flags,
                                  int partFlag) {
        try {
            return (int) MH_PUBLISH_PART.invokeExact(subject, topicId, part,
              flags, partFlag);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_publish_part failed", t);
        }
    }

    // DONT_WAIT-only critical variant. Caller MUST guarantee DONT_WAIT bit set.
    public static int publishPartNoWaitCritical(MemorySegment subject,
                                                MemorySegment topicId,
                                                MemorySegment part, int flags,
                                                int partFlag) {
        try {
            return (int) MH_PUBLISH_PART_CRITICAL.invokeExact(subject, topicId,
              part, flags, partFlag);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_publish_part (critical) failed",
              t);
        }
    }

    public static int subscribe(MemorySegment subject, MemorySegment sourceRidOut,
                                MemorySegment partsOut,
                                MemorySegment partCountOut,
                                MemorySegment topicIdOut,
                                MemorySegment topicIdLenOut,
                                int flags) {
        try {
            NativeMultipartScratch scratch = MULTIPART_RECEIVE_SCRATCH.get();
            scratch.reset();
            return NativeErrno.retryWhileInterrupted(
                () -> subscribeOnce(subject, sourceRidOut, partsOut,
                    partCountOut, topicIdOut, topicIdLenOut, flags, scratch),
                result -> result != 0);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_subscribe_part failed", t);
        }
    }

    private static int subscribeOnce(MemorySegment subject,
                                     MemorySegment sourceRidOut,
                                     MemorySegment partsOut,
                                     MemorySegment partCountOut,
                                     MemorySegment topicIdOut,
                                     MemorySegment topicIdLenOut,
                                     int flags,
                                     NativeMultipartScratch scratch) {
        List<Message> receivedParts = new ArrayList<>();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment routingIdPtrOut = arena.allocate(
                ValueLayout.ADDRESS);
            MemorySegment hasMoreOut = arena.allocate(
                ValueLayout.JAVA_INT);
            long topicCapacity = topicIdLenOut == null
                || topicIdLenOut.address() == 0
                ? 0L
                : Math.max(0L,
                    topicIdLenOut.get(ValueLayout.JAVA_LONG, 0));
            while (true) {
                Message part = new Message();
                boolean success = false;
                try {
                    int rc = subscribePart(subject, routingIdPtrOut,
                        topicIdOut, topicCapacity, topicIdLenOut,
                        InternalAccess.messageNativeHandle(part),
                        hasMoreOut, flags);
                    if (rc != 0) {
                        Message.closeAll(receivedParts);
                        return rc;
                    }
                    success = true;
                    if (receivedParts.isEmpty()) {
                        copyRoutingIdOut(sourceRidOut,
                            routingIdPtrOut.get(ValueLayout.ADDRESS, 0));
                    }
                    InternalAccess.messageFinishReceive(part,
                        hasMoreOut.get(ValueLayout.JAVA_INT, 0) != 0);
                    receivedParts.add(part);
                    if (!InternalAccess.messageMore(part)) {
                        MemorySegment parts =
                            scratch.materializeParts(receivedParts);
                        partsOut.set(ValueLayout.ADDRESS, 0, parts);
                        partCountOut.set(ValueLayout.JAVA_LONG, 0,
                            scratch.partCount());
                        return 0;
                    }
                } finally {
                    if (!success) {
                        try {
                            part.close();
                        } catch (RuntimeException ignored) {
                        }
                    }
                }
            }
        }
    }

    public static int subscribePart(MemorySegment subject,
                                    MemorySegment sourceRidOut,
                                    MemorySegment topicIdOut,
                                    long topicCapacity,
                                    MemorySegment topicIdLenOut,
                                    MemorySegment partOut,
                                    MemorySegment hasMoreOut,
                                    int flags) {
        try {
            return (int) MH_SUBSCRIBE_PART.invokeExact(subject, sourceRidOut,
              topicIdOut, topicCapacity, topicIdLenOut, partOut, hasMoreOut,
              flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_subscribe_part failed", t);
        }
    }

    // DONT_WAIT-only critical variant. Caller MUST guarantee DONT_WAIT set.
    public static int subscribePartNoWaitCritical(MemorySegment subject,
                                    MemorySegment sourceRidOut,
                                    MemorySegment topicIdOut,
                                    long topicCapacity,
                                    MemorySegment topicIdLenOut,
                                    MemorySegment partOut,
                                    MemorySegment hasMoreOut,
                                    int flags) {
        try {
            return (int) MH_SUBSCRIBE_PART_CRITICAL.invokeExact(subject,
              sourceRidOut, topicIdOut, topicCapacity, topicIdLenOut, partOut,
              hasMoreOut, flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_subscribe_part (critical) failed",
              t);
        }
    }

    public static int subscriptionEvent(MemorySegment subject,
                                        MemorySegment sourceRidOut,
                                        MemorySegment subscribedOut,
                                        MemorySegment topicIdOut,
                                        MemorySegment topicIdLenOut,
                                        int flags) {
        try {
            return (int) MH_XPUB_RECV_PART.invokeExact(subject, sourceRidOut,
              subscribedOut, topicIdOut,
              topicIdLenOut.get(ValueLayout.JAVA_LONG, 0), topicIdLenOut,
              flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_xpub_recv_part failed", t);
        }
    }

    public static MemorySegment monitorOpen(MemorySegment socket, int events) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment options = arena.allocate(
              NativeLayouts.SOCKET_MONITOR_OPEN_OPTIONS_LAYOUT);
            options.set(ValueLayout.JAVA_INT,
              NativeLayouts.SOCKET_MONITOR_OPEN_EVENTS_OFFSET, events);
            return (MemorySegment) MH_MONITOR_OPEN.invokeExact(socket, options);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_socket_monitor_open failed", t);
        }
    }

    public static int monitorHandler(MemorySegment monitor,
                                     MemorySegment handler,
                                     MemorySegment userdata) {
        try {
            return (int) MH_MONITOR_HANDLER.invokeExact(monitor, handler,
              userdata);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_socket_monitor_handler failed",
              t);
        }
    }

    public static MonitorEvent monitorRecv(MemorySegment socket, int flags) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment evt = arena.allocate(NativeLayouts.MONITOR_EVENT_LAYOUT);
            int rc = (int) MH_MONITOR_RECV.invokeExact(socket, evt, flags);
            if (rc != 0)
                throw InternalAccess.zlinkExceptionFromLastError(systems.zlink.contracts.errors.ErrorCategory.RECV);
            long event = evt.get(ValueLayout.JAVA_LONG, NativeLayouts.MONITOR_EVENT_OFFSET);
            long value = evt.get(ValueLayout.JAVA_LONG, NativeLayouts.MONITOR_VALUE_OFFSET);
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
            String local = NativeHelpers.fromCString(evt.asSlice(NativeLayouts.MONITOR_LOCAL_OFFSET, 256), 256);
            String remote = NativeHelpers.fromCString(evt.asSlice(NativeLayouts.MONITOR_REMOTE_OFFSET, 256), 256);
            return new MonitorEvent(EnumCodecs.monitorEventTypeFromValue(event), value,
              routingSize == 0 ? java.util.Optional.empty()
                : java.util.Optional.of(RoutingId.from(routing)),
              local, remote);
        } catch (ZlinkException ex) {
            throw ex;
        } catch (Throwable t) {
            throw new RuntimeException("monitor recv failed", t);
        }
    }

    public static int monitorStatus(MemorySegment monitor,
                                      MemorySegment snapshotOut) {
        try {
            return (int) MH_MONITOR_SNAPSHOT.invokeExact(monitor, snapshotOut);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_monitor_status failed", t);
        }
    }

    public static int monitorClose(MemorySegment monitorPtr) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment holder = arena.allocate(ValueLayout.ADDRESS);
            holder.set(ValueLayout.ADDRESS, 0, monitorPtr);
            return (int) MH_MONITOR_CLOSE.invokeExact(holder);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_monitor_close failed", t);
        }
    }

    public static int setTlsServer(MemorySegment handle, MemorySegment cert,
                                   MemorySegment key, int requireClient) {
        try {
            return (int) MH_SET_TLS_SRV.invokeExact(handle, cert, key,
              requireClient);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_set_tls_server failed", t);
        }
    }

    public static int setTlsClient(MemorySegment handle, MemorySegment ca,
                                   MemorySegment host, int trust) {
        try {
            return (int) MH_SET_TLS_CLI.invokeExact(handle, ca, host, trust);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_set_tls_client failed", t);
        }
    }

    public static int errno() {
        try {
            return (int) MH_ERRNO.invokeExact();
        } catch (Throwable t) {
            throw new RuntimeException("zlink_errno failed", t);
        }
    }

    public static String strerror(int errnum) {
        try {
            MemorySegment cstr = (MemorySegment) MH_STRERROR.invokeExact(errnum);
            if (cstr == null || cstr.address() == 0)
                return "";
            return cstr.reinterpret(1024).getString(0);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_strerror failed", t);
        }
    }

    public static int has(MemorySegment capability) {
        try {
            return (int) MH_HAS.invokeExact(capability);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_has failed", t);
        }
    }

    public static void sleep(int seconds) {
        try {
            MH_SLEEP.invokeExact(seconds);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_sleep failed", t);
        }
    }

    public static int proxy(MemorySegment frontend, MemorySegment backend,
                            MemorySegment capture) {
        try {
            return (int) MH_PROXY.invokeExact(frontend, backend, capture);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_proxy failed", t);
        }
    }

    public static int proxySteerable(MemorySegment frontend,
                                     MemorySegment backend,
                                     MemorySegment capture,
                                     MemorySegment control) {
        try {
            return (int) MH_PROXY_STEERABLE.invokeExact(frontend, backend,
                capture, control);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_proxy_steerable failed", t);
        }
    }

    public static MemorySegment atomicCounterNew() {
        try {
            return (MemorySegment) MH_ATOMIC_COUNTER_NEW.invokeExact();
        } catch (Throwable t) {
            throw new RuntimeException("zlink_atomic_counter_new failed", t);
        }
    }

    public static void atomicCounterSet(MemorySegment counter, int value) {
        try {
            MH_ATOMIC_COUNTER_SET.invokeExact(counter, value);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_atomic_counter_set failed", t);
        }
    }

    public static int atomicCounterInc(MemorySegment counter) {
        try {
            return (int) MH_ATOMIC_COUNTER_INC.invokeExact(counter);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_atomic_counter_inc failed", t);
        }
    }

    public static int atomicCounterDec(MemorySegment counter) {
        try {
            return (int) MH_ATOMIC_COUNTER_DEC.invokeExact(counter);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_atomic_counter_dec failed", t);
        }
    }

    public static int atomicCounterValue(MemorySegment counter) {
        try {
            return (int) MH_ATOMIC_COUNTER_VALUE.invokeExact(counter);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_atomic_counter_value failed", t);
        }
    }

    public static void atomicCounterDestroy(MemorySegment counter) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment holder = arena.allocate(ValueLayout.ADDRESS);
            holder.set(ValueLayout.ADDRESS, 0, counter);
            MH_ATOMIC_COUNTER_DESTROY.invokeExact(holder);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_atomic_counter_destroy failed", t);
        }
    }

    public static MemorySegment timerNew() {
        try {
            return (MemorySegment) MH_TIMER_NEW.invokeExact();
        } catch (Throwable t) {
            throw new RuntimeException("zlink_timer_new failed", t);
        }
    }

    public static int timerDestroy(MemorySegment timer) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment holder = arena.allocate(ValueLayout.ADDRESS);
            holder.set(ValueLayout.ADDRESS, 0, timer);
            return (int) MH_TIMER_DESTROY.invokeExact(holder);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_timer_destroy failed", t);
        }
    }

    public static int timerStart(MemorySegment timer, long intervalNs,
                                 long repeatCount) {
        try {
            return (int) MH_TIMER_START.invokeExact(timer, intervalNs,
                repeatCount);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_timer_start failed", t);
        }
    }

    public static int timerStop(MemorySegment timer) {
        try {
            return (int) MH_TIMER_STOP.invokeExact(timer);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_timer_stop failed", t);
        }
    }

    public static int timerRecv(MemorySegment timer, MemorySegment fireCountOut) {
        try {
            return (int) MH_TIMER_RECV.invokeExact(timer, fireCountOut);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_timer_recv failed", t);
        }
    }

    public static int timerHandler(MemorySegment timer, MemorySegment handler,
                                   MemorySegment userdata) {
        try {
            return (int) MH_TIMER_HANDLER.invokeExact(timer, handler, userdata);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_timer_handler failed", t);
        }
    }

    public static MemorySegment stopwatchStart() {
        try {
            return (MemorySegment) MH_STOPWATCH_START.invokeExact();
        } catch (Throwable t) {
            throw new RuntimeException("zlink_stopwatch_start failed", t);
        }
    }

    public static long stopwatchIntermediate(MemorySegment watch) {
        try {
            return (long) MH_STOPWATCH_INTERMEDIATE.invokeExact(watch);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_stopwatch_intermediate failed", t);
        }
    }

    public static long stopwatchStop(MemorySegment watch) {
        try {
            return (long) MH_STOPWATCH_STOP.invokeExact(watch);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_stopwatch_stop failed", t);
        }
    }

    public static MemorySegment threadStart(MemorySegment func,
                                            MemorySegment arg) {
        try {
            return (MemorySegment) MH_THREAD_START.invokeExact(func, arg);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_thread_start failed", t);
        }
    }

    public static void threadJoin(MemorySegment thread) {
        try {
            MH_THREAD_JOIN.invokeExact(thread);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_thread_join failed", t);
        }
    }

    public static int routerRecvPart(MemorySegment router,
                                     MemorySegment sourceNodeRidOut,
                                     MemorySegment requestSeqOut,
                                     MemorySegment partOut,
                                     MemorySegment hasMoreOut,
                                     int flags) {
        try {
            return (int) MH_ROUTER_RECV_PART.invokeExact(router,
                sourceNodeRidOut, requestSeqOut, partOut,
                hasMoreOut, flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_router_recv_part failed", t);
        }
    }

    // DONT_WAIT-only critical variant. Caller MUST guarantee the DONT_WAIT
    // bit is set in flags so that the underlying call is non-blocking.
    public static int routerRecvPartNoWaitCritical(MemorySegment router,
                                                   MemorySegment sourceNodeRidOut,
                                                   MemorySegment requestSeqOut,
                                                   MemorySegment partOut,
                                                   MemorySegment hasMoreOut,
                                                   int flags) {
        try {
            return (int) MH_ROUTER_RECV_PART_CRITICAL.invokeExact(router,
                sourceNodeRidOut, requestSeqOut, partOut,
                hasMoreOut, flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_router_recv_part (critical) failed", t);
        }
    }

    public static int dealerRequestPart(MemorySegment dealer,
                                        MemorySegment part,
                                        int flags,
                                        int partFlag,
                                        int timeoutMs,
                                        MemorySegment handler,
                                        MemorySegment userdata) {
        try {
            return (int) MH_DEALER_REQUEST_PART.invokeExact(dealer, part, flags,
                partFlag, timeoutMs, handler, userdata);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_dealer_request_part failed", t);
        }
    }

    public static int routerRequestPart(MemorySegment router,
                                        MemorySegment peerRid,
                                        MemorySegment part,
                                        int flags,
                                        int partFlag,
                                        int timeoutMs,
                                        MemorySegment handler,
                                        MemorySegment userdata) {
        try {
            return (int) MH_ROUTER_REQUEST_PART.invokeExact(router, peerRid,
                part, flags, partFlag, timeoutMs, handler, userdata);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_router_request_part failed", t);
        }
    }

    public static int routerReplyPart(MemorySegment router,
                                      MemorySegment peerRid,
                                      long requestSeq,
                                      MemorySegment part,
                                      int partFlag) {
        try {
            return (int) MH_ROUTER_REPLY_PART.invokeExact(router, peerRid,
                requestSeq, part, partFlag);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_router_reply_part failed", t);
        }
    }

    public static int routerCompletionControlPart(MemorySegment router,
                                                  MemorySegment peerRid,
                                                  MemorySegment part,
                                                  int partFlag) {
        try {
            return (int) MH_ROUTER_COMPLETION_CONTROL_PART.invokeExact(
                router, peerRid, part, partFlag);
        } catch (Throwable t) {
            throw new RuntimeException(
                "zlink_router_completion_control_part failed", t);
        }
    }

    public static int routerCompletionControlHandler(MemorySegment router,
                                                     MemorySegment handler,
                                                     MemorySegment userData) {
        try {
            return (int) MH_ROUTER_COMPLETION_CONTROL_HANDLER.invokeExact(
                router, handler, userData);
        } catch (Throwable t) {
            throw new RuntimeException(
                "zlink_router_completion_control_handler failed", t);
        }
    }

    public static int pollRaw(MemorySegment items, int count, int timeoutMs) {
        return NativePollerSymbols.pollRaw(items, count, timeoutMs);
    }

    public static MemorySegment pollerNew() {
        return NativePollerSymbols.pollerNew();
    }

    public static int pollerDestroy(MemorySegment pollerPtr) {
        return NativePollerSymbols.pollerDestroy(pollerPtr);
    }

    public static int pollerSize(MemorySegment poller) {
        return NativePollerSymbols.pollerSize(poller);
    }

    public static int pollerAdd(MemorySegment poller, MemorySegment socket,
                                MemorySegment userData, int events) {
        return NativePollerSymbols.pollerAdd(poller, socket, userData, events);
    }

    public static int pollerAddReceiver(MemorySegment poller,
                                         MemorySegment receiver,
                                         MemorySegment userData,
                                         int events) {
        return NativePollerSymbols.pollerAdd(poller, receiver, userData, events);
    }

    public static int pollerAddFd(MemorySegment poller, int fd,
                                  MemorySegment userData, int events) {
        return NativePollerSymbols.pollerAddFd(poller, fd, userData, events);
    }

    public static int pollerAddZlinkTimer(MemorySegment poller, MemorySegment timer,
                                     MemorySegment userData) {
        return NativePollerSymbols.pollerAddTimer(poller, timer, userData);
    }

    public static int pollerModify(MemorySegment poller, MemorySegment socket,
                                   int events) {
        return NativePollerSymbols.pollerModify(poller, socket, events);
    }

    public static int pollerModifyReceiver(MemorySegment poller,
                                            MemorySegment receiver,
                                            int events) {
        return NativePollerSymbols.pollerModify(poller, receiver, events);
    }

    public static int pollerModifyFd(MemorySegment poller, int fd, int events) {
        return NativePollerSymbols.pollerModifyFd(poller, fd, events);
    }

    public static int pollerRemove(MemorySegment poller, MemorySegment socket) {
        return NativePollerSymbols.pollerRemove(poller, socket);
    }

    public static int pollerRemoveReceiver(MemorySegment poller,
                                            MemorySegment receiver) {
        return NativePollerSymbols.pollerRemove(poller, receiver);
    }

    public static int pollerRemoveFd(MemorySegment poller, int fd) {
        return NativePollerSymbols.pollerRemoveFd(poller, fd);
    }

    public static int pollerRemoveZlinkTimer(MemorySegment poller,
                                        MemorySegment timer) {
        return NativePollerSymbols.pollerRemoveTimer(poller, timer);
    }

    public static int pollerWait(MemorySegment poller, MemorySegment events,
                                 int count, int timeoutMs) {
        return NativePollerSymbols.pollerWait(poller, events, count, timeoutMs);
    }

    public static int pollerWait(MemorySegment poller, MemorySegment events,
                                 int count, int timeoutMs,
                                 MemorySegment errorOut) {
        return NativePollerSymbols.pollerWait(poller, events, count, timeoutMs,
            errorOut);
    }

    public static int pollerWait(MemorySegment poller, MemorySegment event,
                                 int timeoutMs) {
        return NativePollerSymbols.pollerWait(poller, event, timeoutMs);
    }
}
