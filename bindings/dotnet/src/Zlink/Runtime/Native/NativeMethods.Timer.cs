using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

internal static partial class NativeMethods
{
    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr zlink_timer_new();

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_timer_destroy(ref IntPtr timer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_timer_start(IntPtr timer,
        ulong intervalNs, ulong repeatCount);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_timer_stop(IntPtr timer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_timer_recv(IntPtr timer,
        out ulong fireCount);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_timer_handler(IntPtr timer,
        ZlinkTimerHandlerDelegate handler, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void ZlinkTimerHandlerDelegate(IntPtr timer,
        ulong fireCount, IntPtr userData);
}
