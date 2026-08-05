// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;

namespace Systems.Zlink;

// Single source of the FNV-1a hash used to key routing-id caches. Kept in one
// place so the algorithm and its constants are not duplicated across the
// RoutingId and RoutingIdCodec cache-key implementations. Aggressively inlined
// so the byte-span and inline-word callers keep their hot-path performance.
internal static class RouteHash
{
    private const ulong Offset = 14695981039346656037UL;
    private const ulong Prime = 1099511628211UL;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal static ulong Fnv1a(ReadOnlySpan<byte> bytes)
    {
        var hash = Offset;
        for (var i = 0; i < bytes.Length; i++)
        {
            hash ^= bytes[i];
            hash *= Prime;
        }

        return hash;
    }

    // Hashes the inline (lo, hi) word representation of a routing id of up to
    // 16 bytes without materializing a byte[], matching Fnv1a byte-for-byte.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal static ulong Fnv1aInline(int size, ulong lo, ulong hi)
    {
        var hash = Offset;
        for (var i = 0; i < size && i < 8; i++)
        {
            hash ^= (lo >> (i * 8)) & 0xFFUL;
            hash *= Prime;
        }

        for (var i = 8; i < size; i++)
        {
            hash ^= (hi >> ((i - 8) * 8)) & 0xFFUL;
            hash *= Prime;
        }

        return hash;
    }
}

internal readonly struct RouteCacheKey : IEquatable<RouteCacheKey>
{
    private RouteCacheKey(int length, ulong hash)
    {
        Length = length;
        Hash = hash;
    }

    internal int Length { get; }
    internal ulong Hash { get; }

    internal static RouteCacheKey FromHash(int length, ulong hash)
    {
        return new RouteCacheKey(length, hash);
    }

    internal static RouteCacheKey Create(ReadOnlySpan<byte> bytes)
    {
        return new RouteCacheKey(bytes.Length, RouteHash.Fnv1a(bytes));
    }

    public bool Equals(RouteCacheKey other)
    {
        return Length == other.Length && Hash == other.Hash;
    }

    public override bool Equals(object? obj)
    {
        return obj is RouteCacheKey other && Equals(other);
    }

    public override int GetHashCode()
    {
        return HashCode.Combine(Length, Hash);
    }
}
