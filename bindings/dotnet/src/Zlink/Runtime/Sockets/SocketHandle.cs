// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed class SocketHandle : IDisposable
{
    private const long Closing = long.MinValue;
    private const long Closed = 1L << 62;
    private readonly IntPtr _handle;
    // A reservation must root the object that owns finalization as well.
    private readonly SocketKernel _lifetimeOwner;
    // Low bits count native calls. Only an idle owner can accept close.
    private long _state;
    private Context? _context;

    public SocketHandle(SocketKernel lifetimeOwner, Context context, SocketType type)
    {
        if (context == null)
            throw new ArgumentNullException(nameof(context));

        _lifetimeOwner = lifetimeOwner;
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
        if (state != 0)
        {
            if ((state & Closed) != 0)
                return;
            throw new ZlinkCloseException(CloseResult.Busy, (int)ErrorCode.EBusy);
        }

        var result = CloseResult.InternalError;
        try
        {
            result = (CloseResult)NativeMethods.zlink_close(_handle);
            if (result != CloseResult.Ok)
                throw ZlinkException.CreateCloseException(result);
        }
        finally
        {
            // Publish Core's outcome before reservations resume. A rejected
            // close leaves the handle usable; an accepted close never does.
            if (result is CloseResult.Ok or CloseResult.Shutdown)
            {
                _context = null;
                Volatile.Write(ref _state, Closed);
            }
            else
                Volatile.Write(ref _state, 0);
            GC.KeepAlive(context);
        }
    }

    internal bool TryReserve(out Call call)
    {
        var spin = new SpinWait();
        var state = Volatile.Read(ref _state);
        while (true)
        {
            if ((state & Closed) != 0)
            {
                call = default;
                return false;
            }
            if ((state & Closing) != 0)
            {
                // Closing may still return BUSY. Observe its published result
                // before deciding whether this call is terminated.
                spin.SpinOnce();
                state = Volatile.Read(ref _state);
                continue;
            }
            var observed = Interlocked.CompareExchange(ref _state,
                state + 1, state);
            if (observed == state)
            {
                call = new Call(this);
                return true;
            }
            state = observed;
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal Call Reserve(bool submit = false)
    {
        if (TryReserve(out var call))
            return call;
        if (submit)
            throw new ZlinkSubmitException(SubmitResult.Terminated,
                (int)ErrorCode.EShutdown);
        throw new ObjectDisposedException(nameof(SocketHandle));
    }

    internal readonly struct Call : IDisposable
    {
        private readonly SocketHandle _owner;

        internal Call(SocketHandle owner)
        {
            _owner = owner;
        }

        internal IntPtr Handle => _owner._handle;

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Dispose()
        {
            Interlocked.Decrement(ref _owner._state);
            // Keep the finalizing owner alive through the last native use.
            GC.KeepAlive(_owner._lifetimeOwner);
        }
    }
}
