using System.Security.Cryptography;

namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkRelocationInventoryHash
{
    internal static string Compute(ReadOnlySpan<byte> bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
}
