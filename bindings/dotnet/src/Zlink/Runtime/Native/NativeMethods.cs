using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

internal static partial class NativeMethods
{
    internal const string LibraryName = "zlink";

    static NativeMethods()
    {
        NativeLibraryLoader.EnsureLoaded();
    }

    internal enum ZlinkPartFlag
    {
        Final = 0,
        More = 1
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void ZlinkStreamOnPacketDelegate(
        IntPtr stream,
        IntPtr routingId,
        IntPtr header,
        IntPtr body,
        IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void ZlinkFreeFnDelegate(IntPtr data, IntPtr hint);
}
