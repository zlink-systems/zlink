// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class AtomicCounter : NativeOwner, IAtomicCounter
{
    public AtomicCounter() : base(CreateHandle())
    {
    }

    public int Value
    {
        get
        {
            EnsureNotDisposed();
            return NativeMethods.zlink_atomic_counter_value(_handle);
        }
    }

    public void Set(int value)
    {
        EnsureNotDisposed();
        NativeMethods.zlink_atomic_counter_set(_handle, value);
    }

    public int Increment()
    {
        EnsureNotDisposed();
        return NativeMethods.zlink_atomic_counter_inc(_handle);
    }

    public int Decrement()
    {
        EnsureNotDisposed();
        return NativeMethods.zlink_atomic_counter_dec(_handle);
    }

    public void Dispose()
    {
        if (IsClosed)
            return;
        NativeMethods.zlink_atomic_counter_destroy(ref _handle);
        MarkClosed();
        GC.SuppressFinalize(this);
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }

    ~AtomicCounter()
    {
        Dispose();
    }

    private void EnsureNotDisposed()
    {
        EnsureNativeHandle(nameof(AtomicCounter));
    }

    private static IntPtr CreateHandle()
    {
        var handle = NativeMethods.zlink_atomic_counter_new();
        if (handle == IntPtr.Zero)
            throw ZlinkException.CreateConfigException(
                NativeMethods.zlink_errno());
        return handle;
    }
}
