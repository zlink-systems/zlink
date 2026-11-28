// SPDX-License-Identifier: MPL-2.0

using System.Text;
using Systems.Zlink.Runtime.Native;
using Systems.Zlink.Runtime.Sockets.Internal;

namespace Systems.Zlink;

internal static class SubscriptionIntrospection
{
    internal static unsafe SubscriptionEntry? At(IntPtr handle, int index)
    {
        if (index < 0)
            throw new ArgumentOutOfRangeException(nameof(index));

        var nativeIndex = (nuint)index;
        Span<byte> buffer = stackalloc byte[SocketOptionAccessor.DefaultOptionBufferSize];
        nuint length = (nuint)buffer.Length;
        int result;
        int isPattern;
        fixed (byte* ptr = buffer)
        {
            result = NativeMethods.zlink_subscription_at(handle, nativeIndex,
                (IntPtr)ptr, ref length, out isPattern);
        }

        if (result == (int)ConfigResult.BufferTooSmall)
        {
            buffer = new byte[checked((int)length)];
            fixed (byte* ptr = buffer)
            {
                result = NativeMethods.zlink_subscription_at(handle, nativeIndex,
                    (IntPtr)ptr, ref length, out isPattern);
            }
        }

        if (result == (int)ConfigResult.NotFound)
            return null;
        ZlinkException.ThrowConfigIfError(result);
        return new SubscriptionEntry(
            Encoding.UTF8.GetString(buffer[..checked((int)length)]), isPattern != 0);
    }
}
