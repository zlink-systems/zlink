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
        return NativeMethods.zlink_stopwatch_intermediate(_handle);
    }

    public ulong Stop()
    {
        EnsureNotDisposed();
        var elapsed = NativeMethods.zlink_stopwatch_stop(_handle);
        MarkClosed();
        GC.SuppressFinalize(this);
        return elapsed;
    }

    public void Dispose()
    {
        if (IsClosed)
            return;
        try
        {
            NativeMethods.zlink_stopwatch_stop(_handle);
        }
        catch
        {
        }

        MarkClosed();
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
                NativeMethods.zlink_errno());
        return handle;
    }
}
