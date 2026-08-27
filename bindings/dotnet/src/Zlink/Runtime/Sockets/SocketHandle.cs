// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed class SocketHandle : IDisposable
{
    private IntPtr _handle;

    public SocketHandle(Context context, SocketType type)
    {
        if (context == null)
            throw new ArgumentNullException(nameof(context));

        _handle = NativeMethods.zlink_socket(context.Handle, (int)type);
        if (_handle == IntPtr.Zero)
            throw ZlinkException.CreateConfigException(
                NativeMethods.zlink_errno());
    }

    public void Dispose()
    {
        if (_handle == IntPtr.Zero)
            return;

        var lastErrno = 0;
        while (true)
        {
            var rc = NativeMethods.zlink_close(_handle);
            if (rc == 0)
            {
                RequestProgressPump.StopSocket(_handle);
                _handle = IntPtr.Zero;
                GC.SuppressFinalize(this);
                return;
            }

            var errno = NativeMethods.zlink_errno();
            lastErrno = errno;
            var code = ZlinkException.MapErrorCode(errno);
            if (code == ErrorCode.EIntr || errno == 4)
                continue;
            throw ZlinkException.CreateCloseException(lastErrno);
        }
    }

    public IntPtr DangerousGetHandle()
    {
        if (_handle == IntPtr.Zero)
            throw new ObjectDisposedException(nameof(SocketHandle));
        return _handle;
    }

    ~SocketHandle()
    {
        try
        {
            Dispose();
        }
        catch
        {
        }
    }
}
