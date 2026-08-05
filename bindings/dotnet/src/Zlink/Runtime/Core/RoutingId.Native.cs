// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

public readonly partial struct RoutingId
{
    private const int ThreadCacheMaxEntries = 256;
    private const int InlineDirectCacheEntries = 256;

    [ThreadStatic] private static Dictionary<RouteCacheKey, List<RouteCacheEntry>>? t_ownedCache;
    [ThreadStatic] private static InlineRouteCacheEntry[]? t_inlineDirectCache;

    internal static RoutingId? FromOptionalBytes(ReadOnlySpan<byte> bytes)
    {
        return bytes.Length == 0 ? null : From(bytes);
    }

    internal byte[] ToByteArray()
    {
        return _bytes?.ToArray() ?? Array.Empty<byte>();
    }

    internal static RoutingId? FromOwnedOptionalBytes(byte[] bytes)
    {
        if (bytes.Length == 0)
            return null;
        Validate(bytes, nameof(bytes));
        return FromOwnedBytesCached(bytes);
    }

    internal static RoutingId? TryFromInlineCached(int size, ulong lo, ulong hi)
    {
        if (size <= 0 || size > 16)
            return null;
        var hash = RouteHash.Fnv1aInline(size, lo, hi);
        var direct = TryFromInlineDirectCache(size, lo, hi, hash);
        if (direct != null)
            return direct;
        var key = RouteCacheKey.FromHash(size, hash);
        var cache = t_ownedCache;
        if (cache == null || !cache.TryGetValue(key,
                out var entries))
            return null;
        for (var i = 0; i < entries.Count; i++)
        {
            var entryBytes = entries[i].Bytes;
            if (entryBytes.Length != size)
                continue;
            if (!InlineMatchesBytes(size, lo, hi, entryBytes))
                continue;
            StoreInlineDirectCache(size, lo, hi, hash, entries[i].RoutingId);
            return entries[i].RoutingId;
        }

        return null;
    }

    private static bool InlineMatchesBytes(int size, ulong lo, ulong hi,
        byte[] entryBytes)
    {
        for (var i = 0; i < size && i < 8; i++)
            if (entryBytes[i] != (byte)(lo >> (i * 8)))
                return false;
        for (var i = 8; i < size; i++)
            if (entryBytes[i] != (byte)(hi >> ((i - 8) * 8)))
                return false;
        return true;
    }

    internal static unsafe RoutingId? FromNative(ref ZlinkRoutingId routingId)
    {
        int size = routingId.Size;
        if (size <= 0)
            return null;

        fixed (byte* src = routingId.Data)
        {
            return FromSpanCached(new ReadOnlySpan<byte>(src, size));
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal byte[] AsByteArrayUnsafe()
    {
        return _bytes ?? Array.Empty<byte>();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal ZlinkRoutingId ToNative()
    {
        return NativeHelpers.WriteRoutingId(ToBytes());
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal ref ZlinkRoutingId ToNativeRef(ref ZlinkRoutingId fallback)
    {
        fallback = NativeHelpers.WriteRoutingId(ToBytes());
        return ref fallback;
    }

    private static RoutingId FromOwnedBytesCached(byte[] bytes)
    {
        var key = RouteCacheKey.Create(bytes);
        var cache =
            t_ownedCache ??= new Dictionary<RouteCacheKey, List<RouteCacheEntry>>();
        if (cache.TryGetValue(key, out var entries))
            for (var i = 0; i < entries.Count; i++)
                if (bytes.AsSpan().SequenceEqual(entries[i].Bytes))
                    return entries[i].RoutingId;

        var created = new RoutingId(bytes, true);
        StoreInlineDirectCache(bytes, key.Hash, created);
        if (cache.Count >= ThreadCacheMaxEntries)
            cache.Clear();
        if (!cache.TryGetValue(key, out entries))
        {
            entries = new List<RouteCacheEntry>(1);
            cache[key] = entries;
        }

        entries.Add(new RouteCacheEntry(bytes, created));
        return created;
    }

    private static RoutingId FromSpanCached(ReadOnlySpan<byte> bytes)
    {
        var key = RouteCacheKey.Create(bytes);
        var cache =
            t_ownedCache ??= new Dictionary<RouteCacheKey, List<RouteCacheEntry>>();
        if (cache.TryGetValue(key, out var entries))
            for (var i = 0; i < entries.Count; i++)
                if (bytes.SequenceEqual(entries[i].Bytes))
                    return entries[i].RoutingId;

        var ownedBytes = bytes.ToArray();
        var created = new RoutingId(ownedBytes, true);
        StoreInlineDirectCache(ownedBytes, key.Hash, created);
        if (cache.Count >= ThreadCacheMaxEntries)
            cache.Clear();
        if (!cache.TryGetValue(key, out entries))
        {
            entries = new List<RouteCacheEntry>(1);
            cache[key] = entries;
        }

        entries.Add(new RouteCacheEntry(ownedBytes, created));
        return created;
    }

    private sealed class RouteCacheEntry
    {
        internal RouteCacheEntry(byte[] bytes, RoutingId routingId)
        {
            Bytes = bytes;
            RoutingId = routingId;
        }

        internal byte[] Bytes { get; }
        internal RoutingId RoutingId { get; }
    }

    private static RoutingId? TryFromInlineDirectCache(int size, ulong lo,
        ulong hi, ulong hash)
    {
        var cache = t_inlineDirectCache;
        if (cache == null)
            return null;

        ref var entry = ref cache[(int)hash
                                  & (InlineDirectCacheEntries - 1)];
        if (entry.RoutingId == null || entry.Size != size
                                    || entry.Hash != hash || entry.Lo != lo || entry.Hi != hi)
            return null;
        return entry.RoutingId;
    }

    private static void StoreInlineDirectCache(byte[] bytes, ulong hash,
        RoutingId routingId)
    {
        if (bytes.Length <= 0 || bytes.Length > 16)
            return;

        ulong lo = 0;
        ulong hi = 0;
        for (var i = 0; i < bytes.Length && i < 8; i++)
            lo |= (ulong)bytes[i] << (i * 8);
        for (var i = 8; i < bytes.Length; i++)
            hi |= (ulong)bytes[i] << ((i - 8) * 8);
        StoreInlineDirectCache(bytes.Length, lo, hi, hash, routingId);
    }

    private static void StoreInlineDirectCache(int size, ulong lo, ulong hi,
        ulong hash, RoutingId routingId)
    {
        var cache = t_inlineDirectCache
            ??= new InlineRouteCacheEntry[InlineDirectCacheEntries];
        cache[(int)hash & (InlineDirectCacheEntries - 1)] =
            new InlineRouteCacheEntry(size, hash, lo, hi, routingId);
    }

    private readonly record struct InlineRouteCacheEntry(
        int Size,
        ulong Hash,
        ulong Lo,
        ulong Hi,
        RoutingId? RoutingId);

}
