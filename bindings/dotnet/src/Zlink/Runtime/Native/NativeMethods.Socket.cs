using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

internal static partial class NativeMethods
{
    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr zlink_socket(IntPtr context, int type);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_bind(IntPtr socket,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string addr);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_connect(IntPtr socket,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string addr);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_unbind(IntPtr socket,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string addr);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_disconnect(IntPtr socket,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string addr);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_disconnect_rid(IntPtr socket,
        ref ZlinkRoutingId peerRoutingId);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static unsafe partial int zlink_send_part(IntPtr socket,
        ref ZlinkMsg part, int flags, ZlinkPartFlag partFlag,
        IntPtr userContext = default, ulong* completionIdOut = null);

    // DONT_WAIT-only variant: same C function, kept as a separate entry point
    // so managed code can choose the non-blocking path explicitly.
    [LibraryImport(LibraryName, EntryPoint = "zlink_send_part")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static unsafe partial int zlink_send_part_nowait(IntPtr socket,
        ref ZlinkMsg part, int flags, ZlinkPartFlag partFlag,
        IntPtr userContext = default, ulong* completionIdOut = null);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int zlink_recv_part(IntPtr socket,
        out IntPtr sourceRoutingId, ref ZlinkMsg part, out int hasMore,
        int flags);

    // DONT_WAIT-only variant: same C function, kept as a separate entry point
    // so managed code can choose the non-blocking path explicitly.
    [LibraryImport(LibraryName, EntryPoint = "zlink_recv_part")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int zlink_recv_part_nowait(IntPtr socket,
        out IntPtr sourceRoutingId, ref ZlinkMsg part, out int hasMore,
        int flags);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_router_recv_part(IntPtr router,
        out IntPtr sourceNodeRoutingId, out ulong replyToken,
        ref ZlinkMsg part, out int hasMore, int flags);

    // DONT_WAIT-only fast variant.
    [DllImport(LibraryName, EntryPoint = "zlink_router_recv_part",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_router_recv_part_nowait(IntPtr router,
        out IntPtr sourceNodeRoutingId, out ulong replyToken,
        ref ZlinkMsg part, out int hasMore, int flags);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static unsafe partial int zlink_send_part_rid(IntPtr handle,
        ref ZlinkRoutingId targetRoutingId, ref ZlinkMsg part, int flags,
        ZlinkPartFlag partFlag, IntPtr userContext = default,
        ulong* completionIdOut = null);

    // DONT_WAIT-only fast variant.
    [LibraryImport(LibraryName, EntryPoint = "zlink_send_part_rid")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static unsafe partial int zlink_send_part_rid_nowait(IntPtr handle,
        ref ZlinkRoutingId targetRoutingId, ref ZlinkMsg part, int flags,
        ZlinkPartFlag partFlag, IntPtr userContext = default,
        ulong* completionIdOut = null);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe int zlink_request_part(IntPtr socket,
        ZlinkRoutingId* targetRouterRoutingIdOrNull, ref ZlinkMsg part,
        int flags, ZlinkPartFlag partFlag, uint timeoutMs,
        IntPtr userContext, ulong* completionIdOut);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_reply_part(IntPtr router,
        ref ZlinkRoutingId sourceRoutingId, ulong replyToken,
        ref ZlinkMsg part, ZlinkPartFlag partFlag);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe int zlink_stream_recv_packet(IntPtr stream,
        out IntPtr sourceRoutingId, ref ZlinkMsg header, ref ZlinkMsg body,
        int flags);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_completion_recv(IntPtr socket,
        ref ZlinkCompletion completion, int flags);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void zlink_completion_close(
        ref ZlinkCompletion completion);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_set_option(IntPtr handle, int option,
        IntPtr value, nuint length);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_get_option(IntPtr handle, int option,
        IntPtr value, ref nuint length);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_set_routing_id(IntPtr handle, IntPtr data,
        nuint size);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_get_routing_id(IntPtr handle,
        out ZlinkRoutingId routingId);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_set_tls_server(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string cert,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
        int requireClientCert);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_set_tls_client(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string caCert,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string hostname,
        int trustSystem);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_socket_set_receive_flow_state(
        IntPtr handle, int state);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_set_router_option(IntPtr handle,
        int option, IntPtr value, nuint length);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_get_router_option(IntPtr handle,
        int option, IntPtr value, ref nuint length);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_set_dealer_option(IntPtr handle,
        int option, IntPtr value, nuint length);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_get_dealer_option(IntPtr handle,
        int option, IntPtr value, ref nuint length);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_set_pub_option(IntPtr handle, int option,
        IntPtr value, nuint length);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_get_pub_option(IntPtr handle, int option,
        IntPtr value, ref nuint length);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_set_sub_option(IntPtr handle, int option,
        IntPtr value, nuint length);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_get_sub_option(IntPtr handle, int option,
        IntPtr value, ref nuint length);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_set_stream_option(IntPtr handle,
        int option, IntPtr value, nuint length);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_get_stream_option(IntPtr handle,
        int option, IntPtr value, ref nuint length);

}
