using System.Security.Cryptography;
using System.Text;
using StackExchange.Redis;

namespace Zlink.Framework.Locations.Redis;

/// <summary>
/// Maps opaque provider keys to one Redis Cluster hash slot.
/// The Framework owns the meaning and encoding of each original key.
/// </summary>
internal sealed class ZLinkRedisLocationKeys
{
    private const string HashTag = "{zlink-location-v3}";
    private readonly string _prefix;

    public ZLinkRedisLocationKeys(string prefix)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(prefix);
        if (prefix.Contains('{', StringComparison.Ordinal)
            || prefix.Contains('}', StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "Redis location key prefix must not contain '{' or '}'.",
                nameof(prefix));
        }

        _prefix = prefix;
    }

    private string Base => $"{_prefix}:{HashTag}";

    public RedisKey OpaqueRecordKey(string key) =>
        $"{Base}:opaque:{Digest(key)}";

    public RedisKey OpaqueIndexKey() =>
        $"{Base}:opaque:index";

    public RedisKey OpaqueMapKey() =>
        $"{Base}:opaque:map";

    public RedisKey OpaqueCleanupKey() =>
        $"{Base}:opaque:cleanup";

    public RedisKey OpaqueSequenceKey() =>
        $"{Base}:opaque:sequence";

    public RedisKey OpaqueSnapshotExpiryKey() =>
        $"{Base}:opaque:snapshot-expiry";

    public RedisKey OpaqueSnapshotBoundaryKey() =>
        $"{Base}:opaque:snapshot-boundary";

    public RedisKey OpaqueScanKey(string scanId) =>
        $"{Base}:opaque:scan:{NormalizeId(scanId)}";

    private static string Digest(string value) =>
        Convert.ToHexString(
                SHA256.HashData(Encoding.UTF8.GetBytes(value)))
            .ToLowerInvariant();

    private static string NormalizeId(string value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value);
        return value.Replace("-", string.Empty, StringComparison.Ordinal)
            .ToLowerInvariant();
    }
}
