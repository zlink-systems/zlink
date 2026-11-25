using System.Security.Cryptography;
using System.Text.Json;

namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkDirectRelocationInventory
{
    // The standalone inventory fits in a single leaf. Property order follows
    // the JCS byte order required by location runtime §8.3.
    internal static string Digest(
        ZLinkAuthorityKey key,
        string originalStoreVersion,
        ReadOnlySpan<byte> targetPayload
    )
    {
        var page = new
        {
            children = Array.Empty<object>(),
            entries = new[]
            {
                new
                {
                    authorityKey = key.Value,
                    authorityPayloadSha256 = Hash(targetPayload),
                    expectedStoreVersion = originalStoreVersion,
                    index = 0,
                    membershipMutationSha256 = Hash("[]"u8),
                    ownerTransition = "newOwner",
                },
            },
            entryCount = 1,
            index = 0,
            level = 0,
            startIndex = 0,
        };
        var root = new
        {
            topLevel = 0,
            topPages = new[]
            {
                new
                {
                    entryCount = 1,
                    index = 0,
                    level = 0,
                    sha256 = Hash(
                        JsonSerializer.SerializeToUtf8Bytes(page, ZLinkCanonicalJson.Options)
                    ),
                    startIndex = 0,
                },
            },
            totalCount = 1,
        };
        return Hash(JsonSerializer.SerializeToUtf8Bytes(root, ZLinkCanonicalJson.Options));
    }

    private static string Hash(ReadOnlySpan<byte> bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
}
