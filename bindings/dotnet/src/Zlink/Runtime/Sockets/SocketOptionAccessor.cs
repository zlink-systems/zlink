// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using System.Text;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed class SocketOptionAccessor
{
    private readonly SocketHandle _handle;

    public SocketOptionAccessor(SocketHandle handle)
    {
        _handle = handle ?? throw new ArgumentNullException(nameof(handle));
    }

    public unsafe void SetInt32(SocketOption option, int value)
    {
        var tmp = value;
        var ptr = new IntPtr(&tmp);
        var rc = SetCore(option, ptr, sizeof(int));
        ZlinkException.ThrowConfigIfError(rc);
    }

    public unsafe void SetInt64(SocketOption option, long value)
    {
        var tmp = value;
        var ptr = new IntPtr(&tmp);
        var rc = SetCore(option, ptr, sizeof(long));
        ZlinkException.ThrowConfigIfError(rc);
    }

    public unsafe void SetUInt64(SocketOption option, ulong value)
    {
        var tmp = value;
        var ptr = new IntPtr(&tmp);
        var rc = SetCore(option, ptr, sizeof(ulong));
        ZlinkException.ThrowConfigIfError(rc);
    }

    public unsafe void SetBytes(SocketOption option, ReadOnlySpan<byte> value)
    {
        var handle = _handle.DangerousGetHandle();
        if (option == SocketOption.RoutingId)
        {
            int rc;
            fixed (byte* ptr = value)
            {
                rc = NativeMethods.zlink_set_routing_id(handle, (IntPtr)ptr,
                    (nuint)value.Length);
            }

            ZlinkException.ThrowConfigIfError(rc);
            return;
        }

        fixed (byte* ptr = value)
        {
            var rc = SetCore(option, (IntPtr)ptr, (nuint)value.Length);
            ZlinkException.ThrowConfigIfError(rc);
        }
    }

    public void SetString(SocketOption option, string value)
    {
        if (value == null)
            throw new ArgumentNullException(nameof(value));

        var maxByteCount = Encoding.UTF8.GetMaxByteCount(value.Length);
        if (maxByteCount <= 512)
        {
            Span<byte> buffer = stackalloc byte[maxByteCount];
            var byteCount = Encoding.UTF8.GetBytes(value.AsSpan(), buffer);
            SetBytes(option, buffer.Slice(0, byteCount));
            return;
        }

        var rented = ArrayPool<byte>.Shared.Rent(maxByteCount);
        try
        {
            var byteCount = Encoding.UTF8.GetBytes(value, rented);
            SetBytes(option, rented.AsSpan(0, byteCount));
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(rented);
        }
    }

    public unsafe int GetInt32(SocketOption option)
    {
        var value = 0;
        var size = (nuint)sizeof(int);
        var ptr = new IntPtr(&value);
        var rc = GetCore(option, ptr, ref size);
        ZlinkException.ThrowConfigIfError(rc);
        return value;
    }

    public unsafe long GetInt64(SocketOption option)
    {
        long value = 0;
        var size = (nuint)sizeof(long);
        var ptr = new IntPtr(&value);
        var rc = GetCore(option, ptr, ref size);
        ZlinkException.ThrowConfigIfError(rc);
        return value;
    }

    public unsafe ulong GetUInt64(SocketOption option)
    {
        ulong value = 0;
        var size = (nuint)sizeof(ulong);
        var ptr = new IntPtr(&value);
        var rc = GetCore(option, ptr, ref size);
        ZlinkException.ThrowConfigIfError(rc);
        return value;
    }

    public byte[] GetBytes(SocketOption option, int initialSize = 256)
    {
        var handle = _handle.DangerousGetHandle();
        if (option == SocketOption.RoutingId)
        {
            var rc = NativeMethods.zlink_get_routing_id(handle, out var routingId);
            ZlinkException.ThrowConfigIfError(rc);
            return NativeHelpers.ReadRoutingId(ref routingId);
        }

        if (initialSize <= 0)
            throw new ArgumentOutOfRangeException(nameof(initialSize));

        var rented = ArrayPool<byte>.Shared.Rent(initialSize);
        try
        {
            while (true)
                unsafe
                {
                    fixed (byte* ptr = rented)
                    {
                        var size = (nuint)rented.Length;
                        var rc = GetCore(option, (IntPtr)ptr, ref size);
                        if (rc == 0)
                            return CopyBytes(rented, size);

                        if (size > (nuint)rented.Length)
                        {
                            ArrayPool<byte>.Shared.Return(rented);
                            rented = ArrayPool<byte>.Shared.Rent(checked((int)size));
                            continue;
                        }

                        ZlinkException.ThrowConfigIfError(rc);
                    }
                }
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(rented);
        }
    }

    public unsafe int GetBytesInto(SocketOption option, Span<byte> destination)
    {
        fixed (byte* ptr = destination)
        {
            var size = (nuint)destination.Length;
            var rc = GetCore(option, (IntPtr)ptr, ref size);
            ZlinkException.ThrowConfigIfError(rc);
            return checked((int)size);
        }
    }

    public string GetString(SocketOption option, int initialSize = 256)
    {
        var bytes = GetBytes(option, initialSize);
        var len = Array.IndexOf(bytes, (byte)0);
        if (len < 0)
            len = bytes.Length;
        return Encoding.UTF8.GetString(bytes, 0, len);
    }

    public static SocketType ReadSocketType(IntPtr handle)
    {
        unsafe
        {
            var value = 0;
            var size = (nuint)sizeof(int);
            var rc = NativeMethods.zlink_get_option(handle, (int)SocketOption.Type,
                new IntPtr(&value), ref size);
            ZlinkException.ThrowConfigIfError(rc);
            return (SocketType)value;
        }
    }

    private int SetCore(SocketOption option, IntPtr value, nuint length)
    {
        var handle = _handle.DangerousGetHandle();
        var code = (int)option;
        if ((code & 0xFF00) == 0x3100)
            return NativeMethods.zlink_set_router_option(handle, code, value,
                length);
        if ((code & 0xFF00) == 0x3200)
            return NativeMethods.zlink_set_dealer_option(handle, code, value,
                length);
        if ((code & 0xFF00) == 0x3300)
            return NativeMethods.zlink_set_pub_option(handle, code, value,
                length);
        if ((code & 0xFF00) == 0x3400)
            return NativeMethods.zlink_set_sub_option(handle, code, value,
                length);
        if ((code & 0xFF00) == 0x3500)
            return NativeMethods.zlink_set_stream_option(handle, code, value,
                length);
        return NativeMethods.zlink_set_option(handle, code, value, length);
    }

    private int GetCore(SocketOption option, IntPtr value, ref nuint length)
    {
        var handle = _handle.DangerousGetHandle();
        var code = (int)option;
        if ((code & 0xFF00) == 0x3100)
            return NativeMethods.zlink_get_router_option(handle, code, value,
                ref length);
        if ((code & 0xFF00) == 0x3200)
            return NativeMethods.zlink_get_dealer_option(handle, code, value,
                ref length);
        if ((code & 0xFF00) == 0x3300)
            return NativeMethods.zlink_get_pub_option(handle, code, value,
                ref length);
        if ((code & 0xFF00) == 0x3400)
            return NativeMethods.zlink_get_sub_option(handle, code, value,
                ref length);
        if ((code & 0xFF00) == 0x3500)
            return NativeMethods.zlink_get_stream_option(handle, code, value,
                ref length);
        return NativeMethods.zlink_get_option(handle, code, value, ref length);
    }

    private static byte[] CopyBytes(byte[] source, nuint size)
    {
        var actual = checked((int)size);
        var result = new byte[actual];
        if (actual == 0)
            return result;

        var toCopy = actual;
        if (toCopy > source.Length)
            toCopy = source.Length;
        Array.Copy(source, result, toCopy);
        return result;
    }
}
