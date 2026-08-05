// SPDX-License-Identifier: MPL-2.0

using System.Collections.Concurrent;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class Timer : NativeOwner, IZlinkTimer
{
    private static readonly ConcurrentDictionary<IntPtr, WeakReference<Timer>>
        TimersByHandle = new();

    private readonly bool _ownsHandle;
    private Action<IZlinkTimer, ulong>? _handler;
    private SynchronizationContext? _handlerContext;
    private NativeMethods.ZlinkTimerHandlerDelegate? _handlerNative;
    private IntPtr _recvPoller;
    private ZlinkPollerEvent[]? _recvPollEvents;

    public Timer() : base(CreateHandle())
    {
        _ownsHandle = true;
        RegisterHandle();
    }

    private Timer(IntPtr handle, bool ownsHandle) : base(handle)
    {
        _ownsHandle = ownsHandle;
        if (ownsHandle)
            RegisterHandle();
    }

    internal IntPtr Handle
    {
        get
        {
            EnsureNotDisposed();
            return _handle;
        }
    }

    public void Start(TimeSpan interval, ulong repeatCount)
    {
        if (interval < TimeSpan.Zero
            || interval.Ticks > (long)(ulong.MaxValue / 100UL))
            throw new ArgumentOutOfRangeException(nameof(interval));

        Start((ulong)interval.Ticks * 100UL, repeatCount);
    }

    public void Stop()
    {
        EnsureNotDisposed();
        var rc = NativeMethods.zlink_timer_stop(_handle);
        if (rc != 0)
            throw ZlinkException.CreateConfigException((ConfigResult)rc);
    }

    public ulong? Recv(RecvFlags flags = RecvFlags.None)
    {
        EnsureNotDisposed();
        if ((flags & RecvFlags.DontWait) != 0 && !PollReadyNoWait())
            return null;

        var rc = NativeMethods.zlink_timer_recv(_handle, out var fireCount);
        if (rc != 0)
            throw ZlinkException.CreateRecvException((RecvResult)rc);
        return fireCount;
    }

    public void OnFire(Action<IZlinkTimer, ulong> handler)
    {
        if (handler == null)
            throw new ArgumentNullException(nameof(handler));
        EnsureNotDisposed();

        _handler = handler;
        _handlerContext = SynchronizationContext.Current;
        if (_handlerNative != null)
            return;

        _handlerNative = OnNativeFire;
        var rc = NativeMethods.zlink_timer_handler(_handle, _handlerNative,
            IntPtr.Zero);
        if (rc != 0)
        {
            _handler = null;
            _handlerContext = null;
            _handlerNative = null;
            throw ZlinkException.CreateHandlerException((HandlerResult)rc);
        }
    }

    public void Close()
    {
        Dispose();
    }

    public void Dispose()
    {
        Destroy(true);
        GC.SuppressFinalize(this);
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }

    internal void Start(ulong intervalNs, ulong repeatCount)
    {
        EnsureNotDisposed();
        var rc = NativeMethods.zlink_timer_start(_handle, intervalNs,
            repeatCount);
        if (rc != 0)
            throw ZlinkException.CreateConfigException((ConfigResult)rc);
    }

    ~Timer()
    {
        Destroy(false);
    }

    private void Destroy(bool throwOnError)
    {
        if (IsClosed)
            return;

        if (!_ownsHandle)
        {
            DestroyRecvPoller(throwOnError);
            MarkClosed();
            ClearHandler();
            return;
        }

        DestroyRecvPoller(throwOnError);
        _ = DestroyHandle(NativeMethods.zlink_timer_destroy, throwOnError,
            originalHandle =>
        {
            UnregisterHandle(originalHandle);
            ClearHandler();
        });
    }

    private void RegisterHandle()
    {
        if (_handle != IntPtr.Zero)
            TimersByHandle[_handle] = new WeakReference<Timer>(this);
    }

    private static void UnregisterHandle(IntPtr handle)
    {
        if (handle != IntPtr.Zero)
            TimersByHandle.TryRemove(handle, out _);
    }

    private bool PollReadyNoWait()
    {
        EnsureRecvPoller();

        var ready = NativeMethods.zlink_poller_wait(_recvPoller,
            _recvPollEvents!, 1, 0, out var errorOut);
        if (ready < 0)
            throw ZlinkException.CreateConfigException((ConfigResult)errorOut);
        return ready > 0;
    }

    private void EnsureRecvPoller()
    {
        if (_recvPoller != IntPtr.Zero)
            return;

        var poller = NativeMethods.zlink_poller_new();
        if (poller == IntPtr.Zero)
            throw ZlinkException.CreateRecvException(NativeMethods.zlink_errno());

        var rc = NativeMethods.zlink_poller_add_timer(poller, _handle,
            IntPtr.Zero);
        if (rc != 0)
        {
            if (poller != IntPtr.Zero)
                _ = NativeMethods.zlink_poller_destroy(ref poller);
            throw ZlinkException.CreateConfigException((ConfigResult)rc);
        }

        _recvPoller = poller;
        _recvPollEvents ??= new ZlinkPollerEvent[1];
    }

    private void DestroyRecvPoller(bool throwOnError)
    {
        if (_recvPoller == IntPtr.Zero)
            return;

        var poller = _recvPoller;
        _ = NativeMethods.zlink_poller_remove_timer(poller, _handle);
        var rc = (CloseResult)NativeMethods.zlink_poller_destroy(ref poller);
        if (rc == CloseResult.Ok)
        {
            _recvPoller = IntPtr.Zero;
            _recvPollEvents = null;
            return;
        }

        if (throwOnError)
            throw ZlinkException.CreateCloseException(rc);
        _recvPoller = IntPtr.Zero;
        _recvPollEvents = null;
    }

    private void EnsureNotDisposed()
    {
        EnsureNativeHandle(nameof(Timer));
    }

    private static IntPtr CreateHandle()
    {
        var handle = NativeMethods.zlink_timer_new();
        if (handle == IntPtr.Zero)
            throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
        return handle;
    }

    private void ClearHandler()
    {
        _handler = null;
        _handlerContext = null;
        _handlerNative = null;
    }

    private void OnNativeFire(IntPtr timer, ulong fireCount, IntPtr userData)
    {
        _ = timer;
        _ = userData;

        var handler = _handler;
        if (handler == null)
            return;

        try
        {
            CallbackDelivery.Post(_handlerContext, () => handler(this, fireCount));
        }
        catch (Exception ex)
        {
            CallbackExceptionHub.Report(ex);
        }
    }
}
