// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class SocketMonitor : NativeOwner, ISocketMonitor
{
    private static readonly NativeMethods.ZlinkMonitorHandlerDelegate NativeIgnore =
        OnIgnoredNativeEvent;

    private static readonly NativeMethods.ZlinkMonitorHandlerDelegate NativeCallback =
        OnNativeEvent;

    public static readonly Action<MonitorEvent> IgnoreHandler = static _ => { };

    private Action<MonitorEvent>? _handler;
    private GCHandle _selfHandle;
    private bool _selfHandleAllocated;

    internal SocketMonitor(IntPtr handle) : base(handle)
    {
        if (handle == IntPtr.Zero)
            throw new ArgumentException("Invalid monitor handle.", nameof(handle));
    }

    internal IntPtr Handle => _handle;

    public void OnEvent(Action<MonitorEvent> handler)
    {
        if (handler == null)
            throw new ArgumentNullException(nameof(handler));
        EnsureNotDisposed();

        var useNativeIgnore = ReferenceEquals(handler, IgnoreHandler);
        _handler = useNativeIgnore ? null : handler;
        if (!useNativeIgnore)
            EnsureSelfHandle();
        var rc = NativeMethods.zlink_socket_monitor_handler(_handle,
            useNativeIgnore ? NativeIgnore : NativeCallback,
            useNativeIgnore ? IntPtr.Zero : GCHandle.ToIntPtr(_selfHandle));
        if (rc != 0)
            throw ZlinkException.CreateHandlerException((HandlerResult)rc);
    }

    public MonitorEvent? Recv(RecvFlags flags = RecvFlags.None)
    {
        EnsureNotDisposed();
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
        EnsureNotDisposed();
        var rc = NativeMethods.zlink_monitor_status(_handle, out var native);
        if (rc != 0)
            throw ZlinkException.CreateConfigException((ConfigResult)rc);
        return MonitorConverters.FromNative(ref native);
    }

    public void Close()
    {
        if (IsClosed)
            return;
        _handler = null;
        _ = DestroyHandle(NativeMethods.zlink_monitor_close,
            throwOnError: true, _ => ReleaseSelfHandle());
    }

    public void Dispose()
    {
        if (_handle == IntPtr.Zero)
            return;
        try
        {
            Close();
        }
        finally
        {
            GC.SuppressFinalize(this);
        }
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }

    internal MonitorEvent? Recv(bool nonBlocking)
    {
        return Recv(nonBlocking ? RecvFlags.DontWait : RecvFlags.None);
    }

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

    private void EnsureNotDisposed()
    {
        EnsureNativeHandle(nameof(SocketMonitor));
    }

    private void EnsureSelfHandle()
    {
        if (_selfHandleAllocated)
            return;

        _selfHandle = GCHandle.Alloc(this, GCHandleType.Normal);
        _selfHandleAllocated = true;
    }

    private void ReleaseSelfHandle()
    {
        if (!_selfHandleAllocated)
            return;

        _selfHandle.Free();
        _selfHandle = default;
        _selfHandleAllocated = false;
    }

    private void OnNativeEventCore(ref ZlinkMonitorEvent native)
    {
        var handler = _handler;
        if (handler == null)
            return;

        try
        {
            var monitorEvent = MonitorConverters.FromNative(ref native);
            handler(monitorEvent);
        }
        catch (Exception ex)
        {
            CallbackExceptionHub.Report(ex);
        }
    }

    private static void OnNativeEvent(ref ZlinkMonitorEvent native, IntPtr userData)
    {
        if (userData == IntPtr.Zero)
            return;

        var handle = GCHandle.FromIntPtr(userData);
        if (handle.Target is SocketMonitor monitor)
            monitor.OnNativeEventCore(ref native);
    }

    private static void OnIgnoredNativeEvent(ref ZlinkMonitorEvent native,
        IntPtr userData)
    {
    }
}
