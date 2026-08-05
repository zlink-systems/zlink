using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

internal static partial class NativeMethods
{
    [LibraryImport(LibraryName, EntryPoint = "zlink_publish_part")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static unsafe partial int zlink_publish_part_utf8(IntPtr subject,
        byte* topicId, ref ZlinkMsg part, int flags, ZlinkPartFlag partFlag);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_set_subscription(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filter);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_unset_subscription(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filter);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int zlink_subscribe_part(IntPtr subject,
        out IntPtr sourceRoutingId, byte[] topicIdBuffer, nuint topicIdCapacity,
        out nuint topicIdLenOut, ref ZlinkMsg part, out int hasMore, int flags);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_xpub_recv_part(IntPtr subject,
        out IntPtr sourceRoutingId, out int subscribed, byte[] topicIdBuffer,
        nuint topicIdCapacity, out nuint topicIdLenOut, int flags);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_subscription_at(IntPtr handle,
        nuint index, IntPtr filterOut, ref nuint filterLength,
        out int isPattern);
}
