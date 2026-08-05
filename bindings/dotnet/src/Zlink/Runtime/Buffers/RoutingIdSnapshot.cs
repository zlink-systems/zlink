// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal unsafe struct RoutingIdSnapshot
{
    private const int InlineCapacity = 16;

    private byte _size;
    private ulong _lo;
    private ulong _hi;
    private byte[]? _large;

    internal bool HasValue => _size > 0;

    internal static RoutingIdSnapshot FromPointer(IntPtr routingIdPtr)
    {
        if (routingIdPtr == IntPtr.Zero)
            return default;

        return FromNative(ref *(ZlinkRoutingId*)routingIdPtr);
    }

    internal static RoutingIdSnapshot FromBytes(byte[]? routingId)
    {
        if (routingId == null || routingId.Length == 0)
            return default;

        return FromSpan(routingId);
    }

    internal static RoutingIdSnapshot FromNative(ref ZlinkRoutingId routingId)
    {
        int size = routingId.Size;
        if (size <= 0)
            return default;

        fixed (byte* src = routingId.Data)
        {
            return FromSpan(new ReadOnlySpan<byte>(src, size));
        }
    }

    private static RoutingIdSnapshot FromSpan(ReadOnlySpan<byte> routingId)
    {
        if (routingId.Length <= 0)
            return default;

        var snapshot = new RoutingIdSnapshot
        {
            _size = checked((byte)routingId.Length)
        };

        if (routingId.Length <= InlineCapacity)
        {
            for (var i = 0; i < routingId.Length && i < 8; i++)
                snapshot._lo |= (ulong)routingId[i] << (i * 8);
            for (var i = 8; i < routingId.Length; i++)
                snapshot._hi |= (ulong)routingId[i] << ((i - 8) * 8);
        }
        else
        {
            snapshot._large = routingId.ToArray();
        }

        return snapshot;
    }

    internal RoutingId? ToRoutingId()
    {
        if (_size <= 0)
            return null;
        // Fast cache lookup using inline (lo, hi, size) — avoids allocating
        // a byte[] when the routing id is already in the thread cache.
        if (_large == null)
        {
            var cached =
                RoutingId.TryFromInlineCached(_size, _lo, _hi);
            if (cached != null)
                return cached;
        }

        var bytes = ToByteArray();
        return bytes == null ? null : RoutingId.FromOwnedOptionalBytes(bytes);
    }

    internal RoutingId? GetOrCreateRoutingId(ref RoutingId? cache)
    {
        if (!HasValue)
            return cache;

        cache ??= ToRoutingId();
        return cache;
    }

    internal byte[]? ToByteArray()
    {
        if (_size <= 0)
            return null;
        if (_large != null)
            return _large;

        var bytes = new byte[_size];
        for (var i = 0; i < _size && i < 8; i++)
            bytes[i] = (byte)(_lo >> (i * 8));
        for (var i = 8; i < _size; i++)
            bytes[i] = (byte)(_hi >> ((i - 8) * 8));
        return bytes;
    }

    internal void WriteNative(ref ZlinkRoutingId target)
    {
        if (_size <= 0)
            throw new ArgumentOutOfRangeException(nameof(target),
                "routingId length must be between 1 and 255 bytes.");

        target = default;
        target.Size = _size;
        if (_large != null)
        {
            fixed (byte* src = _large)
            fixed (byte* dst = target.Data)
            {
                new ReadOnlySpan<byte>(src, _size)
                    .CopyTo(new Span<byte>(dst, _size));
            }

            return;
        }

        for (var i = 0; i < _size && i < 8; i++)
            target.Data[i] = (byte)(_lo >> (i * 8));
        for (var i = 8; i < _size; i++)
            target.Data[i] = (byte)(_hi >> ((i - 8) * 8));
    }
}
