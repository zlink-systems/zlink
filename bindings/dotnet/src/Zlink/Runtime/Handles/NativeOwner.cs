// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal abstract class NativeOwner
{
    protected delegate int NativeDestroy(ref IntPtr handle);

    protected IntPtr _handle;

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

    protected CloseResult DestroyHandle(NativeDestroy destroy,
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

    protected static int RetryWhileInterrupted(Func<int> action,
        out int lastErrno)
    {
        while (true)
        {
            var rc = action();
            if (rc == 0)
            {
                lastErrno = 0;
                return rc;
            }

            lastErrno = Runtime.Native.NativeMethods.zlink_errno();
            var code = ZlinkException.MapErrorCode(lastErrno);
            if (code == ErrorCode.EIntr || lastErrno == 4)
                continue;
            return rc;
        }
    }
}
