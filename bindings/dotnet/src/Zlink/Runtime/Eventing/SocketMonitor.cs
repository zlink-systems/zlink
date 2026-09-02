// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class SocketMonitor : NativeOwner, ISocketMonitor
{
    internal SocketMonitor(IntPtr handle) : base(handle)
    {
        if (handle == IntPtr.Zero)
            throw new ArgumentException("Invalid monitor handle.", nameof(handle));
    }

    internal IntPtr Handle => _handle;

    public MonitorEvent? Recv(RecvFlags flags = RecvFlags.None)
    {
        EnsureNativeHandle(nameof(SocketMonitor));
        var rc = NativeMethods.zlink_socket_monitor_recv(_handle, out var native,
            (flags & RecvFlags.DontWait) != 0 ? 1 : 0);
        if (rc == 0)
            return MonitorConverters.FromNative(ref native);
        if ((flags & RecvFlags.DontWait) != 0
            && (RecvResult)rc == RecvResult.NoData)
            return null;
        throw ZlinkException.CreateRecvException((RecvResult)rc);
    }

    public MonitorStatus Status()
    {
        EnsureNativeHandle(nameof(SocketMonitor));
        var rc = NativeMethods.zlink_monitor_status(_handle, out var native);
        if (rc != 0)
            throw ZlinkException.CreateConfigException((ConfigResult)rc);
        return MonitorConverters.FromNative(ref native);
    }

    public void Close()
    {
        if (!IsClosed)
            _ = DestroyHandle(NativeMethods.zlink_monitor_close, throwOnError: true);
    }

    public void Dispose()
    {
        if (_handle != IntPtr.Zero)
            Close();
        GC.SuppressFinalize(this);
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }

    internal MonitorEvent? Recv(bool nonBlocking) =>
        Recv(nonBlocking ? RecvFlags.DontWait : RecvFlags.None);

    internal bool RecvNoWait(out MonitorEvent? monitorEvent)
    {
        monitorEvent = Recv(true);
        return monitorEvent != null;
    }

    ~SocketMonitor()
    {
        try
        {
            Close();
        }
        catch
        {
        }
    }
}
