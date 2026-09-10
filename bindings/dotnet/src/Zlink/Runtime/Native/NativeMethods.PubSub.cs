using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

internal static partial class NativeMethods
{
    [LibraryImport(LibraryName, EntryPoint = "zlink_publish")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static unsafe partial int zlink_publish_utf8(IntPtr subject,
        byte* topicId, ref ZlinkMsg parts, nuint partCount, int flags);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_set_subscription(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filter);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_unset_subscription(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filter);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int zlink_subscribe(IntPtr subject,
        out IntPtr sourceRoutingId, byte[] topicIdBuffer, nuint topicIdCapacity,
        out nuint topicIdLenOut, ref ZlinkMsg parts, nuint partsCapacity,
        out nuint partCount, int flags);

    // DONT_WAIT returns after one dequeue attempt. Keep this separate from the
    // general import: a blocking subscription receive must retain its normal
    // GC transition even when callers otherwise use the same public API.
    [LibraryImport(LibraryName, EntryPoint = "zlink_subscribe")]
    [SuppressGCTransition]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static unsafe partial int zlink_subscribe_dont_wait(
        IntPtr subject, out IntPtr sourceRoutingId, byte* topicIdBuffer,
        nuint topicIdCapacity, out nuint topicIdLenOut, ref ZlinkMsg parts,
        nuint partsCapacity, out nuint partCount, int flags);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_xpub_recv(IntPtr subject,
        out IntPtr sourceRoutingId, out int subscribed, byte[] topicIdBuffer,
        nuint topicIdCapacity, out nuint topicIdLenOut, int flags);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_subscription_at(IntPtr handle,
        nuint index, IntPtr filterOut, ref nuint filterLength,
        out int isPattern);
}
