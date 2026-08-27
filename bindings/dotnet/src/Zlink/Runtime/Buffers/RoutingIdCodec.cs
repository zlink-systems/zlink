// SPDX-License-Identifier: MPL-2.0

using System.Text;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal static class RoutingIdCodec
{
    private const string HexPrefix = "hex:";
    private const int SharedCacheMaxKeys = 4096;
    private static readonly object ByteToPublicCacheLock = new();
    private static readonly object CanonicalCacheLock = new();

    private static readonly Dictionary<RouteCacheKey, List<RouteCacheEntry>>
        ByteToPublicCache = new();

    private static readonly Dictionary<RouteCacheKey, List<byte[]>>
        ByteCanonicalCache = new();

    internal static string ToPublicString(ReadOnlySpan<byte> routingId)
    {
        if (routingId.Length == 0)
            return string.Empty;

        var key = RouteCacheKey.Create(routingId);
        lock (ByteToPublicCacheLock)
        {
            if (ByteToPublicCache.TryGetValue(key, out var entries))
                for (var i = 0; i < entries.Count; i++)
                    if (routingId.SequenceEqual(entries[i].Bytes))
                        return entries[i].Public;
        }

        var utf8 = Encoding.UTF8.GetString(routingId);
        var publicValue = IsPrintableUtf8Roundtrip(utf8, routingId)
            ? utf8
            : HexPrefix + Convert.ToHexString(routingId);
        var copy = routingId.ToArray();

        lock (ByteToPublicCacheLock)
        {
            TrimByteToPublicCacheIfNeeded();
            if (!ByteToPublicCache.TryGetValue(key, out var entries))
            {
                entries = new List<RouteCacheEntry>(1);
                ByteToPublicCache[key] = entries;
            }

            for (var i = 0; i < entries.Count; i++)
                if (copy.AsSpan().SequenceEqual(entries[i].Bytes))
                    return entries[i].Public;

            entries.Add(new RouteCacheEntry(copy, publicValue));
        }

        return publicValue;
    }

    internal static RoutingId? ToRoutingId(ReadOnlySpan<byte> routingId)
    {
        return routingId.Length == 0 ? null : ToRoutingIdCached(routingId);
    }

    internal static unsafe RoutingId? ToRoutingId(ref ZlinkRoutingId routingId)
    {
        int size = routingId.Size;
        if (size <= 0)
            return null;

        fixed (byte* src = routingId.Data)
        {
            return ToRoutingIdCached(new ReadOnlySpan<byte>(src, size));
        }
    }

    internal static RoutingId? ToRoutingId(byte[] routingId)
    {
        return routingId.Length == 0 ? null : ToRoutingIdCached(routingId);
    }

    internal static unsafe ZlinkRoutingId ToNative(uint routingId)
    {
        ZlinkRoutingId native = default;
        native.Size = 4;
        native.Data[0] = (byte)(routingId >> 24);
        native.Data[1] = (byte)(routingId >> 16);
        native.Data[2] = (byte)(routingId >> 8);
        native.Data[3] = (byte)routingId;
        return native;
    }

    internal static bool TryToUInt32(ReadOnlySpan<byte> routingId,
        out uint value)
    {
        if (routingId.Length != 4)
        {
            value = 0;
            return false;
        }

        value = ((uint)routingId[0] << 24)
                | ((uint)routingId[1] << 16)
                | ((uint)routingId[2] << 8)
                | routingId[3];
        return true;
    }

    internal static byte[] FromPublicString(string routingId, string paramName)
    {
        if (routingId == null)
            throw new ArgumentNullException(paramName);
        if (routingId.Length == 0)
            throw new ArgumentOutOfRangeException(paramName,
                "routingId must not be empty.");

        byte[] bytes;
        if (routingId.StartsWith(HexPrefix, StringComparison.OrdinalIgnoreCase))
        {
            var hex = routingId.Substring(HexPrefix.Length);
            if (hex.Length == 0 || (hex.Length & 1) != 0)
                throw new ArgumentException(
                    "Hex routingId must contain an even number of digits.",
                    paramName);

            try
            {
                bytes = Convert.FromHexString(hex);
            }
            catch (FormatException ex)
            {
                throw new ArgumentException(
                    "Invalid hex routingId format.",
                    paramName, ex);
            }

            if (bytes.Length == 0 || bytes.Length > 255)
                throw new ArgumentOutOfRangeException(paramName,
                    "routingId length must be between 1 and 255 bytes.");
        }
        else
        {
            var byteCount = Encoding.UTF8.GetByteCount(routingId);
            if (byteCount <= 0 || byteCount > 255)
                throw new ArgumentOutOfRangeException(paramName,
                    "routingId UTF-8 length must be between 1 and 255 bytes.");

            bytes = new byte[byteCount];
            Encoding.UTF8.GetBytes(routingId, bytes.AsSpan());
        }

        return bytes;
    }

    private static bool IsPrintableUtf8Roundtrip(string text,
        ReadOnlySpan<byte> original)
    {
        for (var i = 0; i < text.Length; i++)
            if (char.IsControl(text[i]))
                return false;

        var byteCount = Encoding.UTF8.GetByteCount(text);
        if (byteCount != original.Length)
            return false;

        var roundtrip = new byte[byteCount];
        Encoding.UTF8.GetBytes(text, roundtrip.AsSpan());
        return roundtrip.AsSpan().SequenceEqual(original);
    }

    private static byte[] Canonicalize(ReadOnlySpan<byte> routingId)
    {
        var key = RouteCacheKey.Create(routingId);
        lock (CanonicalCacheLock)
        {
            TrimCanonicalCacheIfNeeded();
            if (ByteCanonicalCache.TryGetValue(key,
                    out var cachedEntries))
                for (var i = 0; i < cachedEntries.Count; i++)
                    if (routingId.SequenceEqual(cachedEntries[i]))
                        return cachedEntries[i];

            var copy = routingId.ToArray();
            if (!ByteCanonicalCache.TryGetValue(key, out cachedEntries))
            {
                cachedEntries = new List<byte[]>(1);
                ByteCanonicalCache[key] = cachedEntries;
            }

            cachedEntries.Add(copy);
            return copy;
        }
    }

    private static RoutingId ToRoutingIdCached(ReadOnlySpan<byte> routingId)
    {
        var canonical = Canonicalize(routingId);
        return RoutingId.FromOwnedOptionalBytes(canonical)
               ?? throw new InvalidOperationException("routingId must not be empty.");
    }

    private static void TrimByteToPublicCacheIfNeeded()
    {
        if (ByteToPublicCache.Count < SharedCacheMaxKeys)
            return;

        ByteToPublicCache.Clear();
    }

    private static void TrimCanonicalCacheIfNeeded()
    {
        if (ByteCanonicalCache.Count < SharedCacheMaxKeys)
            return;

        ByteCanonicalCache.Clear();
    }

    private sealed class RouteCacheEntry
    {
        internal RouteCacheEntry(byte[] bytes, string @public)
        {
            Bytes = bytes;
            Public = @public;
        }

        internal byte[] Bytes { get; }
        internal string Public { get; }
    }
}
