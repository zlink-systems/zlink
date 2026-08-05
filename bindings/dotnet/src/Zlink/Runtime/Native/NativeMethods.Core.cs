using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

internal static partial class NativeMethods
{
    // Generated to match the Core public C ABI (core/src/libzlink.vers).
    // Every entry is a symbol this binding P/Invokes AND that the shared
    // library exports; NativeLibraryLoader.ValidateRequiredExports() fails fast
    // when any is missing.
    private static readonly string[] RequiredExportNames =
    {
        "zlink_atomic_counter_dec",
        "zlink_atomic_counter_destroy",
        "zlink_atomic_counter_inc",
        "zlink_atomic_counter_new",
        "zlink_atomic_counter_set",
        "zlink_atomic_counter_value",
        "zlink_bind",
        "zlink_close",
        "zlink_connect",
        "zlink_ctx_auto_hwm_recalculate",
        "zlink_ctx_get",
        "zlink_ctx_new",
        "zlink_ctx_set",
        "zlink_ctx_set_data",
        "zlink_ctx_get_data",
        "zlink_ctx_shutdown",
        "zlink_ctx_term",
        "zlink_dealer_recv_part",
        "zlink_dealer_reply_part",
        "zlink_dealer_request_part",
        "zlink_disconnect",
        "zlink_disconnect_rid",
        "zlink_errno",
        "zlink_get_dealer_option",
        "zlink_get_option",
        "zlink_get_pub_option",
        "zlink_get_router_option",
        "zlink_get_routing_id",
        "zlink_get_stream_option",
        "zlink_get_sub_option",
        "zlink_has",
        "zlink_monitor_close",
        "zlink_monitor_status",
        "zlink_msg_close",
        "zlink_msg_copy",
        "zlink_msg_data",
        "zlink_msg_init",
        "zlink_msg_init_size",
        "zlink_msg_move",
        "zlink_msg_refcnt",
        "zlink_msg_size",
        "zlink_multipart_close",
        "zlink_poll",
        "zlink_poller_add",
        "zlink_poller_add_fd",
        "zlink_poller_add_timer",
        "zlink_poller_destroy",
        "zlink_poller_modify",
        "zlink_poller_modify_fd",
        "zlink_poller_new",
        "zlink_poller_remove",
        "zlink_poller_remove_fd",
        "zlink_poller_remove_timer",
        "zlink_poller_size",
        "zlink_poller_wait",
        "zlink_proxy",
        "zlink_proxy_steerable",
        "zlink_publish_part",
        "zlink_recv_handler",
        "zlink_recv_part",
        "zlink_router_completion_control_handler",
        "zlink_router_completion_control_part",
        "zlink_router_recv_part",
        "zlink_router_reply_part",
        "zlink_router_request_part",
        "zlink_send_part",
        "zlink_send_part_rid",
        "zlink_send_ready_handler",
        "zlink_set_dealer_option",
        "zlink_set_option",
        "zlink_set_pub_option",
        "zlink_set_router_option",
        "zlink_set_routing_id",
        "zlink_set_stream_option",
        "zlink_set_sub_option",
        "zlink_set_subscription",
        "zlink_set_tls_client",
        "zlink_set_tls_server",
        "zlink_sleep",
        "zlink_socket",
        "zlink_socket_monitor_handler",
        "zlink_socket_monitor_open",
        "zlink_socket_monitor_recv",
        "zlink_stopwatch_intermediate",
        "zlink_stopwatch_start",
        "zlink_stopwatch_stop",
        "zlink_stream_packet_handler",
        "zlink_strerror",
        "zlink_subscribe_part",
        "zlink_subscription_at",
        "zlink_timer_destroy",
        "zlink_timer_handler",
        "zlink_timer_new",
        "zlink_timer_recv",
        "zlink_timer_start",
        "zlink_timer_stop",
        "zlink_unbind",
        "zlink_unset_subscription",
        "zlink_version",
        "zlink_xpub_recv_part",
    };

    internal static ReadOnlySpan<string> RequiredExports => RequiredExportNames;

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void zlink_version(out int major, out int minor,
        out int patch);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr zlink_ctx_new();

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_ctx_term(IntPtr context);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_ctx_shutdown(IntPtr context);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_ctx_set(IntPtr context, int option,
        int optval);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_ctx_set_data(IntPtr context, int option,
        byte[] optval, nuint optvallen);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_ctx_set_data(IntPtr context, int option,
        in ulong optval, nuint optvallen);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_ctx_get_data(IntPtr context, int option,
        out ulong optval, ref nuint optvallen);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_ctx_get(IntPtr context, int option,
        out int errorOut);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_ctx_auto_hwm_recalculate(IntPtr context);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_close(IntPtr socket);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int zlink_errno();

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr zlink_strerror(int errnum);

    // HOT PATH: these marked message helpers are bounded, non-allocating, and
    // cannot call managed code. Keep allocating init_size, close, and every
    // potentially blocking transport call on the normal GC transition path.
    [LibraryImport(LibraryName)]
    [SuppressGCTransition]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int zlink_msg_init(ref ZlinkMsg msg);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int zlink_msg_init_size(ref ZlinkMsg msg,
        nuint size);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int zlink_msg_close(ref ZlinkMsg msg);

    [DllImport(LibraryName, EntryPoint = "zlink_msg_close",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_msg_close(IntPtr msg);

    [LibraryImport(LibraryName)]
    [SuppressGCTransition]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int zlink_msg_move(ref ZlinkMsg dest,
        ref ZlinkMsg src);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_msg_copy(ref ZlinkMsg dest,
        ref ZlinkMsg src);

    [LibraryImport(LibraryName)]
    [SuppressGCTransition]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr zlink_msg_data(ref ZlinkMsg msg);

    [DllImport(LibraryName, EntryPoint = "zlink_msg_data",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr zlink_msg_data(IntPtr msg);

    [LibraryImport(LibraryName)]
    [SuppressGCTransition]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial nuint zlink_msg_size(ref ZlinkMsg msg);

    [DllImport(LibraryName, EntryPoint = "zlink_msg_size",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern nuint zlink_msg_size(IntPtr msg);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_msg_refcnt(ref ZlinkMsg msg,
        out int errorOut);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void zlink_multipart_close(IntPtr parts, nuint count);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_dealer_request_part(IntPtr dealer,
        ref ZlinkMsg part, int flags, ZlinkPartFlag partFlag, uint timeoutMs,
        IntPtr handler, IntPtr userData);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_dealer_recv_part(IntPtr dealer,
        out byte messageType, out ulong requestSeq, ref ZlinkMsg part,
        out ZlinkPartFlag hasMore, int flags);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_dealer_reply_part(IntPtr dealer,
        ulong requestSeq, ref ZlinkMsg part, ZlinkPartFlag partFlag);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_router_request_part(IntPtr router,
        ref ZlinkRoutingId peerRoutingId, ref ZlinkMsg part, int flags,
        ZlinkPartFlag partFlag, uint timeoutMs,
        IntPtr handler, IntPtr userData);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_router_reply_part(IntPtr router,
        ref ZlinkRoutingId peerRoutingId, ulong requestSeq,
        ref ZlinkMsg part, ZlinkPartFlag partFlag);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr zlink_atomic_counter_new();

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void zlink_atomic_counter_set(IntPtr counter,
        int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_atomic_counter_inc(IntPtr counter);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_atomic_counter_dec(IntPtr counter);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_atomic_counter_value(IntPtr counter);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void zlink_atomic_counter_destroy(ref IntPtr counter);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr zlink_stopwatch_start();

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ulong zlink_stopwatch_intermediate(IntPtr watch);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ulong zlink_stopwatch_stop(IntPtr watch);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void ZlinkReplyHandlerDelegate(int result, IntPtr parts,
        nuint partCount, IntPtr userData);
}
