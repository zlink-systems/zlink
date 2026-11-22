using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkDirectRelocationInventory
{
    private static readonly JsonSerializerOptions CanonicalJson = new()
    {
        Converters = { new CanonicalStringConverter() },
    };

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
                    sha256 = Hash(JsonSerializer.SerializeToUtf8Bytes(page, CanonicalJson)),
                    startIndex = 0,
                },
            },
            totalCount = 1,
        };
        return Hash(JsonSerializer.SerializeToUtf8Bytes(root, CanonicalJson));
    }

    private static string Hash(ReadOnlySpan<byte> bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    private sealed class CanonicalStringConverter : JsonConverter<string>
    {
        private static readonly UTF8Encoding StrictUtf8 = new(false, true);

        public override string? Read(
            ref Utf8JsonReader reader,
            Type type,
            JsonSerializerOptions options
        ) => reader.GetString();

        public override void Write(
            Utf8JsonWriter writer,
            string value,
            JsonSerializerOptions options
        )
        {
            var json = new StringBuilder(value.Length + 2).Append('"');
            foreach (var character in value)
            {
                switch (character)
                {
                    case '"':
                        json.Append("\\\"");
                        break;
                    case '\\':
                        json.Append("\\\\");
                        break;
                    case '\b':
                        json.Append("\\b");
                        break;
                    case '\t':
                        json.Append("\\t");
                        break;
                    case '\n':
                        json.Append("\\n");
                        break;
                    case '\f':
                        json.Append("\\f");
                        break;
                    case '\r':
                        json.Append("\\r");
                        break;
                    case < ' ':
                        json.Append("\\u").Append(((ushort)character).ToString("x4"));
                        break;
                    default:
                        json.Append(character);
                        break;
                }
            }
            json.Append('"');
            writer.WriteRawValue(StrictUtf8.GetBytes(json.ToString()));
        }
    }
}
