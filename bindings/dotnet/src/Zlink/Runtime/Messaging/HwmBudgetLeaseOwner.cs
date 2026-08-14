// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

// Owns the Core credit leases for one caller-visible receive envelope. The
// native pointers never escape this owner, so every terminal envelope path can
// return the exact origin credit without adding a public lease API.
internal sealed class HwmBudgetLeaseOwner : IDisposable
{
    private IntPtr[] _leases = new IntPtr[4];
    private int _count;
    private int _disposed;

    ~HwmBudgetLeaseOwner()
    {
        DisposeCore();
    }

    internal static void Adopt(ref HwmBudgetLeaseOwner? owner,
        ref IntPtr lease)
    {
        if (lease == IntPtr.Zero)
            return;

        try
        {
            owner ??= new HwmBudgetLeaseOwner();
            owner.Add(lease);
            lease = IntPtr.Zero;
        }
        finally
        {
            // If allocating or growing the managed owner fails, return the
            // just-received credit before propagating the failure.
            ReleaseUnowned(ref lease);
        }
    }

    internal static void ReleaseUnowned(ref IntPtr lease)
    {
        if (lease != IntPtr.Zero)
            NativeMethods.zlink_hwm_budget_lease_release(ref lease);
    }

    public void Dispose()
    {
        DisposeCore();
        GC.SuppressFinalize(this);
    }

    private void Add(IntPtr lease)
    {
        if (Volatile.Read(ref _disposed) != 0)
            throw new ObjectDisposedException(nameof(HwmBudgetLeaseOwner));

        if (_count == _leases.Length)
            Array.Resize(ref _leases, checked(_count * 2));
        _leases[_count++] = lease;
    }

    private void DisposeCore()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;

        for (var index = 0; index < _count; index++)
        {
            var lease = _leases[index];
            _leases[index] = IntPtr.Zero;
            HwmBudgetLeaseOwner.ReleaseUnowned(ref lease);
        }

        _count = 0;
    }
}
