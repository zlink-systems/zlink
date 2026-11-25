using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Zlink.Framework.Runtime.Locations;

internal enum ZLinkRelocationCompletionDelivery
{
    Pending,
    TerminalReceived,
    AlreadyTerminal,
    SourceLeaseExpired,
}

internal sealed record ZLinkRelocationCompletionEntry(
    string ObjectKey,
    string OperationId,
    ulong ReplyRouteId,
    string Reply,
    ZLinkCanonicalRequestSourceFence Source,
    ZLinkRelocationCompletionDelivery Delivery = ZLinkRelocationCompletionDelivery.Pending
)
{
    internal bool MatchesRequest(ZLinkRelocationCompletionEntry other) =>
        Source == other.Source && OperationId == other.OperationId;
}

internal sealed record ZLinkRelocationCompletionRecord(
    Guid RelocationId,
    IReadOnlyList<ZLinkRelocationCompletionEntry> Entries
)
{
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private static readonly JsonSerializerOptions Json = new(ZLinkCanonicalJson.Options)
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };
    internal int PendingDeliveries =>
        Entries.Count(entry => entry.Delivery == ZLinkRelocationCompletionDelivery.Pending);

    internal byte[] Encode() => JsonSerializer.SerializeToUtf8Bytes(ToWire(), Json);

    internal Task EncodeAsync(Stream output, CancellationToken cancellationToken) =>
        JsonSerializer.SerializeAsync(output, ToWire(), Json, cancellationToken);

    internal static async ValueTask<ZLinkRelocationCompletionRecord> DecodeAsync(
        Stream input,
        byte[] expectedHash,
        CancellationToken cancellationToken
    )
    {
        try
        {
            var record = FromWire(
                await JsonSerializer
                    .DeserializeAsync<WireRecord>(input, Json, cancellationToken)
                    .ConfigureAwait(false)
            );
            using var sha256 = SHA256.Create();
            using var canonical = new CryptoStream(Stream.Null, sha256, CryptoStreamMode.Write);
            await record.EncodeAsync(canonical, cancellationToken).ConfigureAwait(false);
            await canonical.FlushFinalBlockAsync(cancellationToken).ConfigureAwait(false);
            if (!expectedHash.AsSpan().SequenceEqual(sha256.Hash))
                throw InvalidRecord();
            return record;
        }
        catch (Exception error)
            when (error
                    is JsonException
                        or InvalidOperationException
                        or ArgumentException
                        or FormatException
                        or OverflowException
            )
        {
            throw InvalidRecord();
        }
    }

    internal static ulong ReservedEntrySize(ZLinkRelocationCompletionEntry entry) =>
        (ulong)
            JsonSerializer
                .SerializeToUtf8Bytes(
                    ToWire(
                        entry with
                        {
                            Delivery = ZLinkRelocationCompletionDelivery.SourceLeaseExpired,
                            Reply = string.Empty,
                        }
                    ),
                    Json
                )
                .Length
        + (ulong)entry.Reply.Length
        + 1;

    private object ToWire() =>
        new
        {
            entries = Entries.Select(ToWire),
            formatVersion = "1",
            relocationId = RelocationId.ToString("D"),
        };

    private static object ToWire(ZLinkRelocationCompletionEntry entry) =>
        new
        {
            delivery = JsonNamingPolicy.CamelCase.ConvertName(entry.Delivery.ToString()),
            objectKey = entry.ObjectKey,
            operationId = entry.OperationId,
            reply = entry.Reply,
            replyRouteId = entry.ReplyRouteId.ToString(CultureInfo.InvariantCulture),
            source = new
            {
                leaseGeneration = entry.Source.OwnerLeaseGeneration.ToString(
                    CultureInfo.InvariantCulture
                ),
                nodeGeneration = entry.Source.NodeGeneration.ToString(CultureInfo.InvariantCulture),
                nodeRid = entry.Source.NodeRid,
                ownerId = entry.Source.OwnerId,
            },
        };

    private static ZLinkRelocationCompletionRecord FromWire(WireRecord? wire)
    {
        if (wire is not { FormatVersion: "1", RelocationId: { } id, Entries: { } values })
            throw InvalidRecord();
        var entries = new List<ZLinkRelocationCompletionEntry>(values.Count);
        var identities = new HashSet<(ZLinkCanonicalRequestSourceFence, string)>();
        byte[] previousKey = [];
        foreach (var value in values)
        {
            if (
                value
                is not {
                    Source: { OwnerId: { } ownerId, NodeRid: { } rid } source,
                    ObjectKey: { } objectKey,
                    OperationId: { } operation,
                    Reply: { } reply,
                }
            )
                throw InvalidRecord();
            var fence = new ZLinkCanonicalRequestSourceFence(
                ownerId,
                NonZero(source.LeaseGeneration),
                rid,
                NonZero(source.NodeGeneration)
            );
            var key = Utf8.GetBytes(objectKey);
            if (
                objectKey.Length < 8
                || !objectKey.StartsWith("zla1:", StringComparison.Ordinal)
                || objectKey[5] is not ('a' or 'u' or 's')
                || objectKey[6] != ':'
                || key.AsSpan().SequenceCompareTo(previousKey) < 0
                || ownerId.Length == 0
                || rid.Length > 510
                || !IsLowerHex(rid)
                || operation.Length != 32
                || !IsLowerHex(operation)
                || operation == "00000000000000000000000000000000"
                || Convert.ToBase64String(Convert.FromBase64String(reply)) != reply
                || !identities.Add((fence, operation))
            )
                throw InvalidRecord();
            previousKey = key;
            var delivery = value.Delivery switch
            {
                "pending" => ZLinkRelocationCompletionDelivery.Pending,
                "terminalReceived" => ZLinkRelocationCompletionDelivery.TerminalReceived,
                "alreadyTerminal" => ZLinkRelocationCompletionDelivery.AlreadyTerminal,
                "sourceLeaseExpired" => ZLinkRelocationCompletionDelivery.SourceLeaseExpired,
                _ => throw InvalidRecord(),
            };
            entries.Add(
                new(objectKey, operation, NonZero(value.ReplyRouteId), reply, fence, delivery)
            );
        }
        return new(Guid.ParseExact(id, "D"), entries);
    }

    private sealed record WireRecord(
        string? FormatVersion,
        string? RelocationId,
        List<WireEntry?>? Entries
    );

    private sealed record WireEntry(
        string? Delivery,
        string? ObjectKey,
        string? OperationId,
        string? Reply,
        string? ReplyRouteId,
        WireSource? Source
    );

    private sealed record WireSource(
        string? LeaseGeneration,
        string? NodeGeneration,
        string? NodeRid,
        string? OwnerId
    );

    internal static int CompareObjectKeys(string left, string right) =>
        Utf8.GetBytes(left).AsSpan().SequenceCompareTo(Utf8.GetBytes(right));

    private static ulong NonZero(string? value) =>
        ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var number)
        && number != 0
            ? number
            : throw InvalidRecord();

    private static bool IsLowerHex(string value) =>
        value.Length > 0
        && value.Length % 2 == 0
        && value.All(character => character is (>= '0' and <= '9') or (>= 'a' and <= 'f'));

    private static ZLinkRelocationDataLostException InvalidRecord() =>
        new("Relocation completion record is invalid.");
}
