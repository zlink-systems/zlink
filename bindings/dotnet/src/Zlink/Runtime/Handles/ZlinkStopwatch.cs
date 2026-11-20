// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class ZlinkStopwatch : NativeOwner, IZlinkStopwatch
{
    public ZlinkStopwatch() : base(CreateHandle())
    {
    }

    public ulong Intermediate()
    {
        EnsureNotDisposed();
        return NativeMethods.zlink_stopwatch_intermediate(_handle).Value;
    }

    public ulong Stop()
    {
        if (!TryBeginClose())
            throw new ObjectDisposedException(nameof(ZlinkStopwatch));
        ulong elapsed;
        try
        {
            elapsed = NativeMethods.zlink_stopwatch_stop(_handle).Value;
            MarkClosed();
        }
        finally
        {
            EndClose();
        }
        GC.SuppressFinalize(this);
        return elapsed;
    }

    public void Dispose()
    {
        if (TryBeginClose())
        {
            try
            {
                NativeMethods.zlink_stopwatch_stop(_handle);
            }
            catch
            {
            }
            finally
            {
                MarkClosed();
                EndClose();
            }
        }
        GC.SuppressFinalize(this);
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }

    ~ZlinkStopwatch()
    {
        Dispose();
    }

    private void EnsureNotDisposed()
    {
        EnsureNativeHandle(nameof(ZlinkStopwatch));
    }

    private static IntPtr CreateHandle()
    {
        var handle = NativeMethods.zlink_stopwatch_start();
        if (handle == IntPtr.Zero)
            throw ZlinkException.CreateConfigException(
                NativeMethods.GetLastPInvokeError());
        return handle;
    }
}
