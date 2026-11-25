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

    private const int MaxPageEntries = 1024;
    private const int MaxPageBytes = 1024 * 1024;
    private static readonly string EmptyMembershipHash = Hash("[]"u8);

    internal static string Digest(
        ZLinkAuthorityKey key,
        string originalStoreVersion,
        ReadOnlySpan<byte> targetPayload
    ) => Digest([new Entry(key, originalStoreVersion, Hash(targetPayload))]);

    internal static string Digest(
        IEnumerable<(
            ZLinkAuthorityKey Key,
            string Version,
            ReadOnlyMemory<byte> Payload
        )> participants
    ) =>
        Digest(
            participants.Select(static participant => new Entry(
                participant.Key,
                participant.Version,
                Hash(participant.Payload.Span)
            ))
        );

    // Property order follows JCS. The page hashes fix the authority keys and
    // their exact StoreVersion values in the source inventory.
    private static string Digest(IEnumerable<Entry> participants)
    {
        var entries = participants
            .OrderBy(static participant => participant.Key.Value, StringComparer.Ordinal)
            .Select(
                static (participant, index) =>
                    new
                    {
                        authorityKey = participant.Key.Value,
                        authorityPayloadSha256 = participant.PayloadHash,
                        expectedStoreVersion = participant.Version,
                        index,
                        membershipMutationSha256 = EmptyMembershipHash,
                        ownerTransition = "newOwner",
                    }
            )
            .ToArray();
        var topPages = Pages(entries, 0, static (start, end) => (start, end - start));
        var topLevel = 0;
        while (topPages.Length > 1)
        {
            var children = topPages;
            topPages = Pages(
                children,
                ++topLevel,
                (start, end) =>
                {
                    var first = children[start].startIndex;
                    var last = children[end - 1];
                    return (first, last.startIndex + last.entryCount - first);
                }
            );
        }
        return Hash(
            JsonSerializer.SerializeToUtf8Bytes(
                new
                {
                    topLevel,
                    topPages,
                    totalCount = entries.Length,
                },
                CanonicalJson
            )
        );
    }

    private static PageReference[] Pages<T>(
        T[] items,
        int level,
        Func<int, int, (int Start, int Count)> range
    )
    {
        var pages = new List<PageReference>();
        for (var first = 0; first < items.Length; )
        {
            var count = Math.Min(MaxPageEntries, items.Length - first);
            var bytes = EncodePage(count);
            if (bytes.Length > MaxPageBytes)
            {
                var low = 1;
                var high = count - 1;
                bytes = EncodePage(low);
                while (low < high)
                {
                    var middle = low + (high - low + 1) / 2;
                    var candidate = EncodePage(middle);
                    if (candidate.Length <= MaxPageBytes)
                    {
                        low = middle;
                        bytes = candidate;
                    }
                    else
                        high = middle - 1;
                }
                count = low;
            }
            var (startIndex, entryCount) = range(first, first + count);
            pages.Add(new(entryCount, pages.Count, level, Hash(bytes), startIndex));
            first += count;

            byte[] EncodePage(int size)
            {
                var (start, length) = range(first, first + size);
                var slice = new ArraySegment<T>(items, first, size);
                var empty = new ArraySegment<T>(Array.Empty<T>());
                return JsonSerializer.SerializeToUtf8Bytes(
                    new
                    {
                        children = level == 0 ? empty : slice,
                        entries = level == 0 ? slice : empty,
                        entryCount = length,
                        index = pages.Count,
                        level,
                        startIndex = start,
                    },
                    CanonicalJson
                );
            }
        }
        return pages.ToArray();
    }

    private sealed record Entry(ZLinkAuthorityKey Key, string Version, string PayloadHash);

    private sealed record PageReference(
        int entryCount,
        int index,
        int level,
        string sha256,
        int startIndex
    );

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
