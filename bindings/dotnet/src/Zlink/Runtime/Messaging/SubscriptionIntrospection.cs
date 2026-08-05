// SPDX-License-Identifier: MPL-2.0

using System.Text;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal static class SubscriptionIntrospection
{
    private const int Enoent = 2;
    private const int Esrch = 3;

    internal static unsafe SubscriptionEntry? At(IntPtr handle, int index)
    {
        if (index < 0)
            throw new ArgumentOutOfRangeException(nameof(index));

        return TryAt(handle, checked((nuint)index), out var entry)
            ? entry
            : null;
    }

    private static unsafe bool TryAt(
        IntPtr handle,
        nuint index,
        out SubscriptionEntry? entry)
    {
        entry = null;
        nuint length = 0;
        var rc = NativeMethods.zlink_subscription_at(handle, index,
            IntPtr.Zero, ref length, out var isPattern);
        if (rc != 0 && length == 0)
        {
            if (IsMissingSubscription())
                return false;
        }

        var buffer = new byte[checked((int)length)];
        if (buffer.Length == 0)
        {
            rc = NativeMethods.zlink_subscription_at(handle, index,
                IntPtr.Zero, ref length, out isPattern);
            if (rc != 0 && IsMissingSubscription())
                return false;
            ZlinkException.ThrowConfigIfError(rc);

            entry = new SubscriptionEntry(string.Empty, isPattern != 0);
            return true;
        }

        fixed (byte* ptr = buffer)
        {
            rc = NativeMethods.zlink_subscription_at(handle, index,
                (IntPtr)ptr, ref length, out isPattern);
        }

        if (rc != 0 && IsMissingSubscription())
            return false;
        ZlinkException.ThrowConfigIfError(rc);

        entry = new SubscriptionEntry(
            Encoding.UTF8.GetString(buffer, 0, checked((int)length)),
            isPattern != 0);
        return true;
    }

    private static bool IsMissingSubscription()
    {
        return NativeMethods.zlink_errno() is Enoent or Esrch;
    }
}
