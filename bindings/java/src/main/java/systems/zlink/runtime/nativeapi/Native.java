package systems.zlink.runtime.nativeapi;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.errors.ZlinkException;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
public final class Native {
    private static final int SEND_DONT_WAIT = 1;
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
    private static final MethodHandle MH_CTX_GET_AUTO_HWM_BUDGET_SNAPSHOT =
            downcall("zlink_ctx_get_auto_hwm_budget_snapshot",
                FunctionDescriptor.of(ValueLayout.JAVA_INT,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_CTX_RESET_AUTO_HWM_BUDGET_METRICS =
            downcall("zlink_ctx_reset_auto_hwm_budget_metrics",
                FunctionDescriptor.of(ValueLayout.JAVA_INT,
                    ValueLayout.ADDRESS));
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
    private static final MethodHandle MH_SEND = downcall("zlink_send",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_LONG,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    // DONT_WAIT-only critical variant. zlink_send is non-blocking when
    // called with DONT_WAIT, so the JVM can elide GC safepoint transitions.
    private static final MethodHandle MH_SEND_CRITICAL =
            downcallCritical("zlink_send",
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_SEND_RID = downcall(
            "zlink_send_rid",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    // DONT_WAIT-only critical variant for routed send.
    private static final MethodHandle MH_SEND_RID_CRITICAL =
            downcallCritical("zlink_send_rid",
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_LONG,
                            ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS));
    private static final MethodHandle MH_REQUEST = downcall(
            "zlink_request",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_REPLY = downcall(
            "zlink_reply",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_LONG,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
    private static final MethodHandle MH_RECV = downcall("zlink_recv",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT));
    // DONT_WAIT-only critical variant. Caller MUST guarantee DONT_WAIT so
    // zlink_recv cannot block while the JVM elides safepoint transitions.
    private static final MethodHandle MH_RECV_CRITICAL =
            downcallCritical("zlink_recv",
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_LONG,
                            ValueLayout.ADDRESS,
                            ValueLayout.JAVA_INT));
    private static final MethodHandle MH_STREAM_RECV_PACKET = downcall(
            "zlink_stream_recv_packet",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_COMPLETION_RECV = downcall(
            "zlink_completion_recv",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    private static final MethodHandle MH_COMPLETION_CLOSE = downcall(
            "zlink_completion_close",
            FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));
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
    private static final MethodHandle MH_SOCKET_SET_RECEIVE_FLOW_STATE =
            downcall("zlink_socket_set_receive_flow_state",
                    FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
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
    private static final MethodHandle MH_PUBLISH = downcall(
            "zlink_publish",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT));
    // DONT_WAIT-only critical variant. zlink_publish is non-blocking when
    // called with DONT_WAIT, so the JVM can elide GC safepoint transitions on
    // the publish hot path (parity with MH_SEND_CRITICAL).
    private static final MethodHandle MH_PUBLISH_CRITICAL =
            downcallCritical("zlink_publish",
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_LONG,
                            ValueLayout.JAVA_INT));
    private static final MethodHandle MH_SUBSCRIBE = downcall(
            "zlink_subscribe",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_LONG, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_LONG,
                    ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT));
    // DONT_WAIT-only critical variant. zlink_subscribe is non-blocking
    // when called with DONT_WAIT, so the JVM can elide GC safepoint
    // transitions on the subscribe hot path (parity with
    // MH_RECV_CRITICAL).
    private static final MethodHandle MH_SUBSCRIBE_CRITICAL =
            downcallCritical("zlink_subscribe",
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_LONG,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.JAVA_LONG, ValueLayout.ADDRESS,
                            ValueLayout.JAVA_INT));
    private static final MethodHandle MH_XPUB_RECV = downcall(
            "zlink_xpub_recv",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_LONG,
                    ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT));

    private static final MethodHandle MH_MONITOR_OPEN = downcall("zlink_socket_monitor_open",
            FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
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

    private static final MethodHandle MH_ROUTER_RECV = downcall(
      "zlink_router_recv",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
        ValueLayout.JAVA_LONG, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    // DONT_WAIT-only critical variant. zlink_router_recv is non-blocking
    // when called with DONT_WAIT flag, so the JVM can elide GC safepoint
    // transition for this call. Caller must guarantee DONT_WAIT bit is set.
    private static final MethodHandle MH_ROUTER_RECV_CRITICAL =
      downcallCritical(
        "zlink_router_recv",
        FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
          ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
          ValueLayout.JAVA_LONG, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

    private Native() {}

    private static boolean invalidMultipart(MemorySegment parts,
                                            long partCount) {
        return partCount <= 0
            || parts == null
            || parts.address() == 0;
    }

    private static void copyRoutingIdOut(MemorySegment target,
                                         MemorySegment routingIdPtr) {
        if (target == null || target.address() == 0) {
            return;
        }
        target.set(ValueLayout.ADDRESS, 0,
            routingIdPtr == null ? MemorySegment.NULL : routingIdPtr);
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

    public static int ctxGetAutoHwmBudgetSnapshot(MemorySegment ctx,
                                                   MemorySegment snapshot) {
        try {
            return (int) MH_CTX_GET_AUTO_HWM_BUDGET_SNAPSHOT.invokeExact(
                ctx, snapshot);
        } catch (Throwable t) {
            throw new RuntimeException(
                "zlink_ctx_get_auto_hwm_budget_snapshot failed", t);
        }
    }

    public static int ctxResetAutoHwmBudgetMetrics(MemorySegment ctx) {
        try {
            return (int) MH_CTX_RESET_AUTO_HWM_BUDGET_METRICS.invokeExact(ctx);
        } catch (Throwable t) {
            throw new RuntimeException(
                "zlink_ctx_reset_auto_hwm_budget_metrics failed", t);
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

    public static int sendMultipart(MemorySegment socket, MemorySegment parts,
                                    long partCount, int flags) {
        rejectUntrackedDontWait(flags);
        return send(socket, parts, partCount, flags, MemorySegment.NULL,
            MemorySegment.NULL);
    }

    public static int send(MemorySegment socket, MemorySegment parts,
                           long partCount, int flags,
                           MemorySegment userContext,
                           MemorySegment completionIdOut) {
        requireTrackedDontWait(flags, userContext, completionIdOut);
        try {
            return (int) MH_SEND.invokeExact(socket, parts, partCount, flags,
                userContext, completionIdOut);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_send failed", t);
        }
    }

    // DONT_WAIT-only critical variant. Caller MUST guarantee DONT_WAIT bit set.
    public static int sendNoWaitCritical(MemorySegment socket,
                                         MemorySegment parts,
                                         long partCount, int flags,
                                         MemorySegment userContext,
                                         MemorySegment completionIdOut) {
        requireTrackedDontWait(flags, userContext, completionIdOut);
        try {
            return (int) MH_SEND_CRITICAL.invokeExact(socket, parts, partCount,
                flags, userContext, completionIdOut);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_send (critical) failed", t);
        }
    }

    public static int sendMultipart(MemorySegment socket, MemorySegment routingId,
                                    MemorySegment parts, long partCount,
                                    int flags) {
        rejectUntrackedDontWait(flags);
        return sendRid(socket, routingId, parts, partCount, flags,
            MemorySegment.NULL, MemorySegment.NULL);
    }

    public static int sendRid(MemorySegment socket,
                              MemorySegment routingId,
                              MemorySegment parts, long partCount,
                              int flags, MemorySegment userContext,
                              MemorySegment completionIdOut) {
        requireTrackedDontWait(flags, userContext, completionIdOut);
        try {
            return (int) MH_SEND_RID.invokeExact(socket, routingId, parts,
                partCount, flags, userContext, completionIdOut);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_send_rid failed", t);
        }
    }

    // DONT_WAIT-only critical variant for routed send.
    public static int sendRidNoWaitCritical(MemorySegment socket,
                                            MemorySegment routingId,
                                            MemorySegment parts,
                                            long partCount, int flags,
                                            MemorySegment userContext,
                                            MemorySegment completionIdOut) {
        requireTrackedDontWait(flags, userContext, completionIdOut);
        try {
            return (int) MH_SEND_RID_CRITICAL.invokeExact(socket,
                routingId, parts, partCount, flags, userContext,
                completionIdOut);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_send_rid (critical) failed",
                t);
        }
    }

    public static int sendMultipartU32(MemorySegment socket, int routingId,
                                       MemorySegment parts, long partCount,
                                       int flags) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment nativeRoutingId = arena.allocate(
                NativeLayouts.ROUTING_ID_LAYOUT);
            nativeRoutingId.set(ValueLayout.JAVA_BYTE,
                NativeLayouts.ROUTING_ID_SIZE_OFFSET, (byte) 4);
            nativeRoutingId.set(ValueLayout.JAVA_BYTE,
                NativeLayouts.ROUTING_ID_DATA_OFFSET,
                (byte) (routingId >>> 24));
            nativeRoutingId.set(ValueLayout.JAVA_BYTE,
                NativeLayouts.ROUTING_ID_DATA_OFFSET + 1,
                (byte) (routingId >>> 16));
            nativeRoutingId.set(ValueLayout.JAVA_BYTE,
                NativeLayouts.ROUTING_ID_DATA_OFFSET + 2,
                (byte) (routingId >>> 8));
            nativeRoutingId.set(ValueLayout.JAVA_BYTE,
                NativeLayouts.ROUTING_ID_DATA_OFFSET + 3,
                (byte) routingId);
            return sendMultipart(socket, nativeRoutingId, parts, partCount,
                flags);
        }
    }

    private static void requireTrackedDontWait(
            int flags, MemorySegment userContext,
            MemorySegment completionIdOut) {
        if ((flags & SEND_DONT_WAIT) == 0) {
            return;
        }
        if (userContext == null || userContext.address() == 0L
                || completionIdOut == null
                || completionIdOut.address() == 0L) {
            throw new IllegalArgumentException(
                "DONTWAIT send requires user context and completion ID output");
        }
    }

    private static void rejectUntrackedDontWait(int flags) {
        if ((flags & SEND_DONT_WAIT) != 0) {
            throw new IllegalArgumentException(
                "DONTWAIT send requires token-aware overload");
        }
    }

    public static int recv(MemorySegment socket, MemorySegment sourceRidOut,
                           MemorySegment partsOut,
                           MemorySegment partCountOut,
                           int flags) {
        NativeMultipartScratch scratch = MULTIPART_RECEIVE_SCRATCH.get();
        try {
            scratch.reset();
            for (;;) {
                int rc = (int) MH_RECV.invokeExact(socket,
                    scratch.nodeRidPtrOut, scratch.parts(), scratch.capacity(),
                    scratch.countOut, flags);
                if (rc != RecvResult.BUFFER_TOO_SMALL.value()) {
                    if (rc == RecvResult.OK.value()) {
                        copyRoutingIdOut(sourceRidOut,
                            scratch.nodeRidPtrOut.get(ValueLayout.ADDRESS, 0));
                        partsOut.set(ValueLayout.ADDRESS, 0, scratch.parts());
                        partCountOut.set(ValueLayout.JAVA_LONG, 0,
                            scratch.requiredCount());
                    }
                    return rc;
                }
                scratch.grow(scratch.requiredCount());
            }
        } catch (Throwable t) {
            throw new RuntimeException("zlink_recv failed", t);
        }
    }

    public static int recvNoWaitCritical(MemorySegment socket,
                                         MemorySegment sourceRidOut,
                                         MemorySegment partsOut,
                                         MemorySegment partCountOut,
                                         int flags) {
        NativeMultipartScratch scratch = MULTIPART_RECEIVE_SCRATCH.get();
        try {
            scratch.reset();
            for (;;) {
                int rc = (int) MH_RECV_CRITICAL.invokeExact(socket,
                    scratch.nodeRidPtrOut, scratch.parts(), scratch.capacity(),
                    scratch.countOut, flags);
                if (rc != RecvResult.BUFFER_TOO_SMALL.value()) {
                    if (rc == RecvResult.OK.value()) {
                        copyRoutingIdOut(sourceRidOut,
                            scratch.nodeRidPtrOut.get(ValueLayout.ADDRESS, 0));
                        partsOut.set(ValueLayout.ADDRESS, 0, scratch.parts());
                        partCountOut.set(ValueLayout.JAVA_LONG, 0,
                            scratch.requiredCount());
                    }
                    return rc;
                }
                scratch.grow(scratch.requiredCount());
            }
        } catch (Throwable t) {
            throw new RuntimeException("zlink_recv (critical) failed", t);
        }
    }

    public static int request(MemorySegment socket,
                              MemorySegment targetOrNull,
                              MemorySegment parts, long partCount,
                              int flags, int timeoutMs,
                              MemorySegment userContext,
                              MemorySegment completionIdOut) {
        try {
            return (int) MH_REQUEST.invokeExact(socket, targetOrNull,
                parts, partCount, flags, timeoutMs, userContext,
                completionIdOut);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_request failed", t);
        }
    }

    public static int reply(MemorySegment router, MemorySegment sourceRid,
                            long replyToken, MemorySegment parts,
                            long partCount) {
        try {
            return (int) MH_REPLY.invokeExact(router, sourceRid,
                replyToken, parts, partCount);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_reply failed", t);
        }
    }

    public static int streamRecvPacket(MemorySegment stream,
                                       MemorySegment sourceRidOut,
                                       MemorySegment headerOut,
                                       MemorySegment bodyOut,
                                       int flags) {
        try {
            return (int) MH_STREAM_RECV_PACKET.invokeExact(stream,
                sourceRidOut, headerOut, bodyOut, flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_stream_recv_packet failed", t);
        }
    }

    public static int completionRecv(MemorySegment socket,
                                     MemorySegment completionOut,
                                     int flags) {
        try {
            return (int) MH_COMPLETION_RECV.invokeExact(socket,
                completionOut, flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_completion_recv failed", t);
        }
    }

    public static void completionClose(MemorySegment completion) {
        try {
            MH_COMPLETION_CLOSE.invokeExact(completion);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_completion_close failed", t);
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

    public static int setReceiveFlowState(MemorySegment handle, int state) {
        try {
            return (int) MH_SOCKET_SET_RECEIVE_FLOW_STATE.invokeExact(handle, state);
        } catch (Throwable t) {
            throw new RuntimeException(
                "zlink_socket_set_receive_flow_state failed", t);
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
            return (int) MH_PUBLISH.invokeExact(subject, topicId, parts,
                partCount, flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_publish failed", t);
        }
    }

    public static int publishNoWaitCritical(MemorySegment subject,
                                            MemorySegment topicId,
                                            MemorySegment parts,
                                            long partCount, int flags) {
        try {
            return (int) MH_PUBLISH_CRITICAL.invokeExact(subject, topicId,
                parts, partCount, flags);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_publish (critical) failed",
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
            long topicCapacity = topicIdLenOut == null
                || topicIdLenOut.address() == 0
                ? 0L : Math.max(0L,
                    topicIdLenOut.get(ValueLayout.JAVA_LONG, 0));
            MethodHandle receiver = (flags & SEND_DONT_WAIT) != 0
                ? MH_SUBSCRIBE_CRITICAL : MH_SUBSCRIBE;
            for (;;) {
                int rc = (int) receiver.invokeExact(subject,
                    scratch.nodeRidPtrOut, topicIdOut, topicCapacity,
                    topicIdLenOut, scratch.parts(), scratch.capacity(),
                    scratch.countOut, flags);
                if (rc != RecvResult.BUFFER_TOO_SMALL.value()
                    || scratch.requiredCount() <= scratch.capacity()) {
                    if (rc == RecvResult.OK.value()) {
                        copyRoutingIdOut(sourceRidOut,
                            scratch.nodeRidPtrOut.get(ValueLayout.ADDRESS, 0));
                        partsOut.set(ValueLayout.ADDRESS, 0, scratch.parts());
                        partCountOut.set(ValueLayout.JAVA_LONG, 0,
                            scratch.requiredCount());
                    }
                    return rc;
                }
                scratch.grow(scratch.requiredCount());
            }
        } catch (Throwable t) {
            throw new RuntimeException("zlink_subscribe failed", t);
        }
    }

    public static int subscriptionEvent(MemorySegment subject,
                                        MemorySegment sourceRidOut,
                                        MemorySegment subscribedOut,
                                        MemorySegment topicIdOut,
                                        MemorySegment topicIdLenOut,
                                        int flags) {
        NativeMultipartScratch scratch = MULTIPART_RECEIVE_SCRATCH.get();
        try {
            int rc = (int) MH_XPUB_RECV.invokeExact(subject,
              scratch.nodeRidPtrOut,
              subscribedOut, topicIdOut,
              topicIdLenOut.get(ValueLayout.JAVA_LONG, 0), topicIdLenOut,
              flags);
            if (rc == RecvResult.OK.value()) {
                NativeRoutingIds.copyTo(sourceRidOut,
                    scratch.nodeRidPtrOut.get(ValueLayout.ADDRESS, 0));
            }
            return rc;
        } catch (Throwable t) {
            throw new RuntimeException("zlink_xpub_recv failed", t);
        }
    }

    public static MemorySegment monitorOpen(MemorySegment socket, int events,
                                            long monitorHwmBytes) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment options = arena.allocate(
              NativeLayouts.SOCKET_MONITOR_OPEN_OPTIONS_LAYOUT);
            options.set(ValueLayout.JAVA_INT,
              NativeLayouts.SOCKET_MONITOR_OPEN_EVENTS_OFFSET, events);
            options.set(ValueLayout.JAVA_LONG,
              NativeLayouts.SOCKET_MONITOR_OPEN_HWM_BYTES_OFFSET,
              monitorHwmBytes);
            return (MemorySegment) MH_MONITOR_OPEN.invokeExact(socket, options);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_socket_monitor_open failed", t);
        }
    }

    public static MonitorEvent monitorRecv(MemorySegment socket, int flags) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment evt = arena.allocate(NativeLayouts.MONITOR_EVENT_LAYOUT);
            int rc = (int) MH_MONITOR_RECV.invokeExact(socket, evt, flags);
            if (rc != 0) {
                throw new ZlinkRecvException(RecvResult.fromValue(rc),
                    Native.errno());
            }
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
            long connectionId = evt.get(ValueLayout.JAVA_LONG,
              NativeLayouts.MONITOR_CONNECTION_ID_OFFSET);
            int transportLane = evt.get(ValueLayout.JAVA_INT,
              NativeLayouts.MONITOR_TRANSPORT_LANE_OFFSET);
            int eventFlags = evt.get(ValueLayout.JAVA_INT,
              NativeLayouts.MONITOR_FLAGS_OFFSET);
            return new MonitorEvent(EnumCodecs.monitorEventTypeFromValue(event), value,
              routingSize == 0 ? java.util.Optional.empty()
                : java.util.Optional.of(RoutingId.from(routing)),
              local, remote, connectionId, transportLane, eventFlags);
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

    public static int routerRecv(MemorySegment router,
                                 MemorySegment sourceNodeRidOut,
                                 MemorySegment replyTokenValueOut,
                                 MemorySegment partsOut,
                                 MemorySegment partCountOut,
                                 int flags) {
        NativeMultipartScratch scratch = MULTIPART_RECEIVE_SCRATCH.get();
        try {
            scratch.reset();
            for (;;) {
                int rc = (int) MH_ROUTER_RECV.invokeExact(router,
                    scratch.nodeRidPtrOut, replyTokenValueOut, scratch.parts(),
                    scratch.capacity(), scratch.countOut, flags);
                if (rc != RecvResult.BUFFER_TOO_SMALL.value()) {
                    if (rc == RecvResult.OK.value()) {
                        copyRoutingIdOut(sourceNodeRidOut,
                            scratch.nodeRidPtrOut.get(ValueLayout.ADDRESS, 0));
                        partsOut.set(ValueLayout.ADDRESS, 0, scratch.parts());
                        partCountOut.set(ValueLayout.JAVA_LONG, 0,
                            scratch.requiredCount());
                    }
                    return rc;
                }
                scratch.grow(scratch.requiredCount());
            }
        } catch (Throwable t) {
            throw new RuntimeException("zlink_router_recv failed", t);
        }
    }

    // DONT_WAIT-only critical variant. Caller MUST guarantee the DONT_WAIT
    // bit is set in flags so that the underlying call is non-blocking.
    public static int routerRecvNoWaitCritical(
            MemorySegment router, MemorySegment sourceNodeRidOut,
            MemorySegment replyTokenValueOut, MemorySegment partsOut,
            MemorySegment partCountOut, int flags) {
        NativeMultipartScratch scratch = MULTIPART_RECEIVE_SCRATCH.get();
        try {
            scratch.reset();
            for (;;) {
                int rc = (int) MH_ROUTER_RECV_CRITICAL.invokeExact(router,
                    scratch.nodeRidPtrOut, replyTokenValueOut, scratch.parts(),
                    scratch.capacity(), scratch.countOut, flags);
                if (rc != RecvResult.BUFFER_TOO_SMALL.value()) {
                    if (rc == RecvResult.OK.value()) {
                        copyRoutingIdOut(sourceNodeRidOut,
                            scratch.nodeRidPtrOut.get(ValueLayout.ADDRESS, 0));
                        partsOut.set(ValueLayout.ADDRESS, 0, scratch.parts());
                        partCountOut.set(ValueLayout.JAVA_LONG, 0,
                            scratch.requiredCount());
                    }
                    return rc;
                }
                scratch.grow(scratch.requiredCount());
            }
        } catch (Throwable t) {
            throw new RuntimeException("zlink_router_recv (critical) failed", t);
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
