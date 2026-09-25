using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

internal static partial class NativeMethods
{
    private static readonly string[] RequiredPollerExportNames =
    {
        "zlink_poller_new",
        "zlink_poller_destroy",
        "zlink_poller_size",
        "zlink_poller_add",
        "zlink_poller_add_fd",
        "zlink_poller_add_timer",
        "zlink_poller_modify",
        "zlink_poller_modify_fd",
        "zlink_poller_remove",
        "zlink_poller_remove_fd",
        "zlink_poller_remove_timer",
        "zlink_poller_wait"
    };

    internal static ReadOnlySpan<string> RequiredPollerExports =>
        RequiredPollerExportNames;

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
        SetLastError = true)]
    internal static extern IntPtr zlink_poller_new();

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poller_destroy(ref IntPtr poller);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poller_size(IntPtr poller,
        out int errorOut);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poller_add(IntPtr poller, IntPtr socket,
        IntPtr userData, short events);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poller_add_fd(IntPtr poller, int fd,
        IntPtr userData, short events);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poller_add_timer(IntPtr poller,
        IntPtr timer, IntPtr userData);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poller_modify(IntPtr poller, IntPtr socket,
        short events);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poller_modify_fd(IntPtr poller, int fd,
        short events);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poller_remove(IntPtr poller, IntPtr socket);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poller_remove_fd(IntPtr poller, int fd);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poller_remove_timer(IntPtr poller,
        IntPtr timer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
        SetLastError = true)]
    internal static extern int zlink_poller_wait(IntPtr poller,
        [Out] ZlinkPollerEvent[] events, int nEvents, long timeout,
        out int errorOut);

    [DllImport(LibraryName, EntryPoint = "zlink_poller_wait",
        CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
    internal static extern unsafe int zlink_poller_wait_pinned(IntPtr poller,
        ZlinkPollerEvent* events, int nEvents, long timeout,
        out int errorOut);

    [DllImport(LibraryName, EntryPoint = "zlink_poll",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poll(
        [In] [Out] ZlinkPollItemUnix[] items, int nitems, long timeout,
        out int errorOut);

    [DllImport(LibraryName, EntryPoint = "zlink_poll",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_poll(
        [In] [Out] ZlinkPollItemWindows[] items, int nitems, long timeout,
        out int errorOut);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
        SetLastError = true)]
    internal static extern int zlink_proxy(IntPtr frontend, IntPtr backend,
        IntPtr capture);

    // zlink_has returns a native C++ `bool` (one byte; see core/include/zlink/core/api.h
    // and core/doc/spec/core/07-utilities.{en,ko}.md). It never fails and never sets
    // errno, so the return must be marshaled as U1 (reads only the byte the callee
    // guarantees), not the platform `int` — widening to `int` reads whatever garbage
    // is left in the rest of the register and can surface as a spurious negative rc.
    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    internal static extern bool zlink_has(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string capability);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void zlink_sleep(int seconds);
}
