using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

internal static partial class NativeMethods
{
    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr zlink_socket_monitor_open(
        IntPtr socket, in ZlinkSocketMonitorOpenOptions options);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr zlink_socket_monitor_open(
        IntPtr socket, IntPtr options);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_socket_monitor_recv(IntPtr monitor,
        out ZlinkMonitorEvent @event, int flags);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_monitor_status(IntPtr monitor,
        out ZlinkMonitorStatus snapshot);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int zlink_monitor_close(ref IntPtr monitor);
}
