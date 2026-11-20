// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal abstract class NativeOwner
{
    protected delegate int NativeDestroy(ref IntPtr handle);

    protected IntPtr _handle;

    // One owner at a time may run the native close. Losers wait for the
    // owner's outcome, so a native destroy never runs twice on one handle.
    private int _closing;

    protected NativeOwner(IntPtr handle)
    {
        _handle = handle;
    }

    protected bool IsClosed => _handle == IntPtr.Zero;

    protected void EnsureNativeHandle(string objectName)
    {
        if (_handle == IntPtr.Zero)
            throw new ObjectDisposedException(objectName);
    }

    protected void MarkClosed()
    {
        _handle = IntPtr.Zero;
    }

    protected void RestoreHandle(IntPtr handle)
    {
        _handle = handle;
    }

    // Returns true when the caller now owns the close. False means the handle
    // is already closed (possibly by the owner we waited for). The owner must
    // call EndClose in a finally block.
    protected bool TryBeginClose()
    {
        var spinner = new SpinWait();
        while (true)
        {
            if (Volatile.Read(ref _handle) == IntPtr.Zero)
                return false;
            if (Interlocked.CompareExchange(ref _closing, 1, 0) == 0)
            {
                if (Volatile.Read(ref _handle) != IntPtr.Zero)
                    return true;
                Volatile.Write(ref _closing, 0);
                return false;
            }
            spinner.SpinOnce();
        }
    }

    protected void EndClose()
    {
        Volatile.Write(ref _closing, 0);
    }

    protected CloseResult DestroyHandle(NativeDestroy destroy,
        bool throwOnError, Action<IntPtr>? afterClosed = null)
    {
        if (!TryBeginClose())
            return CloseResult.Ok;
        try
        {
            return DestroyClaimed(destroy, throwOnError, afterClosed);
        }
        finally
        {
            EndClose();
        }
    }

    protected CloseResult DestroyClaimed(NativeDestroy destroy,
        bool throwOnError, Action<IntPtr>? afterClosed = null)
    {
        if (_handle == IntPtr.Zero)
            return CloseResult.Ok;

        var originalHandle = _handle;
        var handle = originalHandle;
        var rc = (CloseResult)destroy(ref handle);
        if (rc == CloseResult.Ok)
        {
            _handle = IntPtr.Zero;
            afterClosed?.Invoke(originalHandle);
            return rc;
        }

        _handle = originalHandle;
        if (throwOnError)
            throw ZlinkException.CreateCloseException(rc);
        return rc;
    }

}
