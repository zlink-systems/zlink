// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed class SocketHandle : IDisposable
{
    private readonly IntPtr _handle;
    private Context? _context;

    // The low bits count native calls. Close can claim only an idle handle.
    private long _state;
    private const long Closing = long.MinValue;
    private const long Closed = 1L << 62;

    public SocketHandle(Context context, SocketType type)
    {
        if (context == null)
            throw new ArgumentNullException(nameof(context));

        _context = context;
        _handle = NativeMethods.zlink_socket(context.Handle, (int)type);
        if (_handle == IntPtr.Zero)
        {
            _context = null;
            throw ZlinkException.CreateConfigException(
                NativeMethods.GetLastPInvokeError());
        }
    }

    public void Dispose()
    {
        var context = _context;
        var state = Interlocked.CompareExchange(ref _state, Closing, 0);
        if (!IsOpen(state))
        {
            GC.KeepAlive(context);
            return;
        }
        if (state != 0)
            throw new ZlinkCloseException(CloseResult.Busy, (int)ErrorCode.EBusy);

        var outcome = 0L;
        try
        {
            var rc = NativeMethods.zlink_close(_handle);
            if (rc == 0 || (CloseResult)rc == CloseResult.Shutdown)
            {
                outcome = Closed;
                _context = null;
            }
            if (rc != 0)
                throw ZlinkException.CreateCloseException((CloseResult)rc);
        }
        finally
        {
            Volatile.Write(ref _state, outcome);
            GC.KeepAlive(context);
        }
    }

    public IntPtr DangerousGetHandle()
    {
        if (!IsOpen(Volatile.Read(ref _state)))
            throw new ObjectDisposedException(nameof(SocketHandle));
        return _handle;
    }

    internal IntPtr NativeHandle => _handle;
    internal bool IsClosed => !IsOpen(Volatile.Read(ref _state));

    internal bool TryReserve()
    {
        while (true)
        {
            var state = ReadSettledState();
            if (!IsOpen(state))
                return false;
            if (Interlocked.CompareExchange(ref _state, state + 1, state) == state)
                return true;
        }
    }

    private long ReadSettledState()
    {
        var spinner = new SpinWait();
        var state = Volatile.Read(ref _state);
        // A rejected close leaves the socket open; observe its result before
        // deciding whether a new call can reserve the handle.
        while ((state & Closing) != 0)
        {
            spinner.SpinOnce();
            state = Volatile.Read(ref _state);
        }
        return state;
    }

    private static bool IsOpen(long state) => (state & Closed) == 0;

    internal NativeCall ReserveSubmit()
    {
        RequireSubmitOpen(TryReserve());
        return new NativeCall(this);
    }

    internal NativeCall ReserveReceive()
    {
        if (!TryReserve())
            throw new ZlinkRecvException(RecvResult.InvalidState, (int)ErrorCode.EShutdown);
        return new NativeCall(this);
    }

    internal void EnsureSubmitOpen() => RequireSubmitOpen(IsOpen(ReadSettledState()));

    private static void RequireSubmitOpen(bool open)
    {
        if (!open)
            throw new ZlinkSubmitException(SubmitResult.Terminated, (int)ErrorCode.EShutdown);
    }

    internal void Release() => Interlocked.Decrement(ref _state);

    internal readonly ref struct NativeCall(SocketHandle owner)
    {
        internal IntPtr Handle => owner._handle;

        public void Dispose() => owner.Release();
    }
}
