using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

internal static partial class NativeMethods
{
    internal const string LibraryName = "zlink";

    static NativeMethods()
    {
        NativeLibraryLoader.EnsureLoaded();
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void ZlinkFreeFnDelegate(IntPtr data, IntPtr hint);
}
