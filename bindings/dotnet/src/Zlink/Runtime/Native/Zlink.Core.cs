// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

public static partial class Zlink
{
    private static string StrerrorCore(int errnum)
    {
        var ptr = NativeMethods.zlink_strerror(errnum);
        return ptr == IntPtr.Zero
            ? string.Empty
            : Marshal.PtrToStringAnsi(ptr)
              ?? string.Empty;
    }

    private static (int Major, int Minor, int Patch) VersionCore()
    {
        NativeMethods.zlink_version(out var major, out var minor, out var patch);
        return (major, minor, patch);
    }

    private static bool HasCore(string capability)
    {
        if (capability == null)
            throw new ArgumentNullException(nameof(capability));

        var rc = NativeMethods.zlink_has(capability);
        if (rc < 0)
            throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
        return rc != 0;
    }

    private static void ProxyCore(IZlinkSocket frontend, IZlinkSocket backend,
        IZlinkSocket? capture)
    {
        var frontendSocket = SocketInterop.RequireSocket(frontend,
            nameof(frontend));
        var backendSocket = SocketInterop.RequireSocket(backend,
            nameof(backend));
        var captureSocket = capture == null
            ? null
            : SocketInterop.RequireSocket(capture, nameof(capture));

        var rc = NativeMethods.zlink_proxy(frontendSocket.Handle,
            backendSocket.Handle, captureSocket?.Handle ?? IntPtr.Zero);
        if (rc != 0)
            throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
    }

    private static void ProxySteerableCore(IZlinkSocket frontend,
        IZlinkSocket backend, IZlinkSocket? capture, IZlinkSocket control)
    {
        var frontendSocket = SocketInterop.RequireSocket(frontend,
            nameof(frontend));
        var backendSocket = SocketInterop.RequireSocket(backend,
            nameof(backend));
        var captureSocket = capture == null
            ? null
            : SocketInterop.RequireSocket(capture, nameof(capture));
        var controlSocket = SocketInterop.RequireSocket(control,
            nameof(control));

        var rc = NativeMethods.zlink_proxy_steerable(frontendSocket.Handle,
            backendSocket.Handle, captureSocket?.Handle ?? IntPtr.Zero,
            controlSocket.Handle);
        if (rc != 0)
            throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
    }

    private static void SleepCore(TimeSpan duration)
    {
        var totalSeconds = duration.TotalSeconds;
        if (double.IsNaN(totalSeconds) || double.IsInfinity(totalSeconds)
                                       || totalSeconds < 0 || totalSeconds > int.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(duration));

        NativeMethods.zlink_sleep((int)Math.Ceiling(totalSeconds));
    }

    private static void MultipartCloseCore(IReadOnlyList<Message> parts)
    {
        if (parts == null)
            throw new ArgumentNullException(nameof(parts));
        foreach (var part in parts)
            part?.Dispose();
    }
}
